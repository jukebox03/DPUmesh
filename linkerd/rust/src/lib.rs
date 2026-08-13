//! linkerd2-proxy behind the DPUmesh L7 adapter contract.
//!
//! The proxy's own datapath is not built (`dmesh-doca` without
//! `own-datapath`); DPUmesh owns the DOCA device, the progress engines and the
//! DMA rings, and reaches the proxy through `dmesh_l7.h`. What is reused is
//! everything above the transport: `DmeshIo` as the connection endpoint, and
//! the acceptor that drives it through the real outbound stack.
//!
//! Threading follows the contract. One `current_thread` runtime per ARM worker,
//! created on that worker's own thread and advanced only from
//! `l7_worker_step`, so no task ever moves between threads and the worker loop
//! keeps owning the iteration.
//!
//! What this adapter supports today is narrower than the contract: one worker
//! carries the proxy, and one active session at a time per service address. A
//! connection outside that is declined with a reason, which the data plane
//! counts and forwards at L4. `linkerd/CONTRACT.md` states the boundary.

use std::cell::Cell;
use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::os::raw::{c_char, c_int};

use dmesh_doca::{DmeshEvent, DmeshIoHandle, FlowId, Registration};
use tokio::sync::mpsc;

/// Mirrors `struct dmesh_l7_flow`. Layout is checked against the C definition
/// by `abi` below and by `tests/l7_abi_contract_test.c`.
#[repr(C)]
pub struct DmeshL7Flow {
    pub src_ip: u32,
    pub dst_ip: u32,
    pub src_port: u16,
    pub dst_port: u16,
    pub src_pod: i32,
    pub dst_service: i32,
    pub peer_pod: i32,
    pub mode: u8,
    pub is_reply: u8,
    pub workload: [c_char; 64],
}

/// Mirrors `struct dmesh_l7_verdict`.
#[repr(C)]
pub struct DmeshL7Verdict {
    pub allow: c_int,
    pub backend_pod: i32,
}

const MODE_OPAQUE: u8 = 2;
const MODE_FULL: u8 = 3;
const BACKEND_ANY: i32 = -1;
/// Mirrors `DMESH_L7_ORIGIN`: return the bytes to the connection's sender
/// rather than forwarding them onward.
const BACKEND_ORIGIN: i32 = -2;

/// Mirrors `DMESH_L7_DECLINE_*`. The data plane counts a fallback by the reason
/// returned here, so these values are wire ABI with `dmesh_l7.h`.
const DECLINE_ERROR: c_int = -1;
const DECLINE_NOT_ATTACHED: c_int = -2;
const DECLINE_MODE: c_int = -3;
const DECLINE_SESSION_LIMIT: c_int = -4;
const DECLINE_UNKNOWN_REPLY: c_int = -5;

/// The pod staging region a segment points into. DPUmesh hands the region base
/// with every segment, so the span only has to cover the largest pod buffer.
const STAGING_SPAN: usize = 64 * 1024 * 1024;

/// Bytes drained from one connection's write buffer per step. The endpoint's
/// own buffer holds 256 KiB (`DEFAULT_TX_CAPACITY` in the port's `io.rs`)
/// before the stack sees backpressure, so a step moves at most a quarter of a
/// full buffer and the worker loop gets the iteration back.
const TX_DRAIN_MAX: usize = 64 * 1024;

/// Bytes one `l7_worker_step` publishes across every session it visits, and
/// sessions it visits at all. Without a budget a session that always has output
/// would hold the worker loop; what is left re-enters on the next step, and
/// `drain_next` rotates the starting point so the same session is not always
/// the one served.
const STEP_DRAIN_MAX: usize = 256 * 1024;
const STEP_SESSIONS_MAX: usize = 64;

/// The port every service address carries. DPUmesh routes on service ids, so
/// the port is not a routing input; it only completes the socket address the
/// proxy needs.
const SERVICE_PORT: u16 = 9092;

/// The DPUmesh half of the contract. Production calls the C entry points named
/// in `dmesh_l7.h`; under `cfg(test)` a recording fake stands in, which is what
/// lets custody and backpressure be exercised without a datapath underneath.
/// The exported `l7_*` symbols are the same either way.
mod datapath {
    #[cfg(test)]
    pub use fake::{release, send};

    #[cfg(not(test))]
    use std::os::raw::c_int;

    #[cfg(not(test))]
    extern "C" {
        fn dmesh_l7_send(
            worker_id: c_int,
            conn: u64,
            backend_pod: i32,
            buf: *const u8,
            len: usize,
        ) -> c_int;
        fn dmesh_l7_release(worker_id: c_int, conn: u64, pos: u32, len: u32);
    }

    #[cfg(not(test))]
    pub fn send(worker_id: c_int, conn: u64, backend_pod: i32, buf: &[u8]) -> c_int {
        unsafe { dmesh_l7_send(worker_id, conn, backend_pod, buf.as_ptr(), buf.len()) }
    }

    #[cfg(not(test))]
    pub fn release(worker_id: c_int, conn: u64, pos: u32, len: u32) {
        unsafe { dmesh_l7_release(worker_id, conn, pos, len) }
    }

    #[cfg(test)]
    pub mod fake {
        use std::cell::RefCell;
        use std::os::raw::c_int;

        /// What the datapath was asked to do, and what it is told to answer.
        #[derive(Default)]
        pub struct Recorded {
            pub sent: Vec<(u64, i32, Vec<u8>)>,
            pub released: Vec<(u64, u32, u32)>,
            /// Bytes the next send accepts; `None` accepts everything offered.
            pub accept: Option<usize>,
            /// Accept one more byte than offered, which the contract forbids.
            pub over_accept: bool,
            /// Answer terminally.
            pub fail: bool,
        }

        thread_local! {
            pub static STATE: RefCell<Recorded> = RefCell::new(Recorded::default());
        }

        pub fn reset() {
            STATE.with(|s| *s.borrow_mut() = Recorded::default());
        }

        pub fn send(_worker_id: c_int, conn: u64, backend_pod: i32, buf: &[u8]) -> c_int {
            STATE.with(|s| {
                let mut s = s.borrow_mut();
                if s.fail {
                    return -1;
                }
                if s.over_accept {
                    return buf.len() as c_int + 1;
                }
                let n = s.accept.unwrap_or(buf.len()).min(buf.len());
                if n > 0 {
                    s.sent.push((conn, backend_pod, buf[..n].to_vec()));
                }
                n as c_int
            })
        }

        pub fn release(_worker_id: c_int, conn: u64, pos: u32, len: u32) {
            STATE.with(|s| s.borrow_mut().released.push((conn, pos, len)));
        }
    }
}

/// What the adapter did, so that a run can be shown to have gone through the
/// proxy rather than around it. Per worker; nothing is shared.
#[derive(Default)]
struct Counters {
    connections_opened: u64,
    connections_closed: u64,
    connections_declined: u64,
    reply_connections_attached: u64,
    bytes_into_linkerd: u64,
    bytes_to_backend: u64,
    bytes_to_origin: u64,
    segments_released: u64,
    send_retries: u64,
    send_errors: u64,
}

impl Counters {
    fn summary(&self) -> String {
        format!(
            "opened={} closed={} declined={} replies={} into_linkerd={} \
             to_backend={} to_origin={} released={} retries={} errors={}",
            self.connections_opened,
            self.connections_closed,
            self.connections_declined,
            self.reply_connections_attached,
            self.bytes_into_linkerd,
            self.bytes_to_backend,
            self.bytes_to_origin,
            self.segments_released,
            self.send_retries,
            self.send_errors,
        )
    }
}

/// The first occurrence and then every 4096th. A decline repeats once per
/// connection, and the DPU log is the only place a bring-up failure shows.
fn rate_limited(count: u64) -> bool {
    count == 1 || count.is_multiple_of(4096)
}

/// Why a connection was refused. The data plane counts a fallback by the code
/// returned here and names it with the same word this does.
#[derive(Clone, Copy)]
enum Decline {
    Error,
    SessionLimit,
    UnknownReply,
}

impl Decline {
    fn code(self) -> c_int {
        match self {
            Decline::Error => DECLINE_ERROR,
            Decline::SessionLimit => DECLINE_SESSION_LIMIT,
            Decline::UnknownReply => DECLINE_UNKNOWN_REPLY,
        }
    }

    fn reason(self) -> &'static str {
        match self {
            Decline::Error => "adapter-error",
            Decline::SessionLimit => "single-session-limit",
            Decline::UnknownReply => "unknown-reply",
        }
    }
}

/// One end of a session as DPUmesh sees it, paired with the `DmeshIo` handle
/// that carries its bytes.
#[derive(Default)]
struct Side {
    conn: Option<u64>,
    handle: Option<DmeshIoHandle>,
    staging_set: bool,
    /// Extents handed over and not yet released. `DmeshIo` copies out of
    /// staging when the stack reads, and reports no per-extent completion, so
    /// custody is returned once the whole pushed queue has drained.
    outstanding: Vec<(u32, u32)>,
}

impl Side {
    /// Give back custody for every extent this side still holds. Release is
    /// mandatory: an extent never returned pins the sender's slot for good, so
    /// this runs on every path that ends the side, not only on a clean drain.
    /// Extents released here are removed, so a later call cannot release them
    /// a second time.
    fn release_outstanding(&mut self, worker_id: c_int, counters: &mut Counters) -> usize {
        let Some(conn) = self.conn else {
            // Nothing to release to: the connection is already gone, and the
            // data plane reclaimed its window with it.
            self.outstanding.clear();
            return 0;
        };
        let n = self.outstanding.len();
        for (pos, len) in self.outstanding.drain(..) {
            datapath::release(worker_id, conn, pos, len);
            counters.segments_released += 1;
        }
        n
    }

    /// Detach the DPUmesh connection from this side, returning what it still
    /// holds first. The next connection to arrive here may stage in a different
    /// pod's region, so the staging base is re-taken with its first segment.
    fn detach(&mut self, worker_id: c_int, counters: &mut Counters) {
        self.release_outstanding(worker_id, counters);
        self.conn = None;
        self.staging_set = false;
    }
}

/// A client connection and the backend channel the proxy reaches through it.
///
/// One DPUmesh connection carries both directions. Bytes arriving on it are
/// read by the proxy from the *client* endpoint; what the proxy writes towards
/// the backend travels onward on that same connection, and what it writes back
/// to the client returns along it. The reply connection, when the backend
/// opens one, feeds the *backend* endpoint and publishes nothing.
struct Session {
    slot: usize,
    /// The endpoint the acceptor built: the proxy's view of the client.
    client: Side,
    /// The endpoint published for the connector: the proxy's view of the backend.
    backend: Side,
    backend_addr: SocketAddr,
}

struct Worker {
    id: c_int,
    /// The proxy's drain signal. Dropping it shuts the proxy down, so it lives
    /// as long as the worker does.
    _drain: Box<dyn std::any::Any>,
    rt: tokio::runtime::Runtime,
    events: mpsc::UnboundedSender<DmeshEvent>,
    registrations: mpsc::UnboundedReceiver<Registration>,
    /// Keyed by the client connection's handle: the session both directions
    /// belong to.
    sessions: HashMap<u64, Session>,
    /// DPUmesh connection handle -> the session it belongs to.
    by_conn: HashMap<u64, u64>,
    /// Session keys in the order they opened, and where the next step starts.
    /// A `HashMap` gives no order to rotate over, and the step budget below
    /// needs one so a session cannot be starved by the sessions ahead of it.
    order: Vec<u64>,
    drain_next: usize,
    /// Slot numbers name a connection to the acceptor; they are ours to issue.
    next_slot: usize,
    pending: HashMap<usize, u64>,
    counters: Counters,
}

/// The client connection's handle, as DPUmesh forms it: pod in the high bits,
/// port in the low. A reply names the same pair through `peer_pod`/`dst_port`.
/// This is `dmesh_l7_conn_handle()` in `dmesh_l7.h`; the two are checked against
/// the same vectors by `session_key_matches_c_handle` and by
/// `tests/l7_abi_contract_test.c`.
fn session_key(pod: i32, port: u16) -> u64 {
    ((pod as u8 as u64) << 16) | port as u64
}

/// The synthetic address a service is reached at. The proxy routes on socket
/// addresses and DPUmesh on service identifiers, so the identifier stands in
/// for the address one-for-one. The range is not loopback: an outbound proxy
/// refuses to originate a connection there.
fn service_addr_v4(dst_service: i32) -> SocketAddrV4 {
    SocketAddrV4::new(
        Ipv4Addr::new(10, 96, 0, (dst_service & 0xff) as u8),
        SERVICE_PORT,
    )
}

/// The same address as the backend registry keys it.
fn service_addr(dst_service: i32) -> SocketAddr {
    SocketAddr::V4(service_addr_v4(dst_service))
}

/// The address a pod is seen as. `src_port` of zero is not a source address the
/// proxy accepts, so a connection without one is given port 1.
fn pod_addr(src_pod: i32, src_port: u16) -> SocketAddrV4 {
    SocketAddrV4::new(
        Ipv4Addr::new(10, 97, 0, (src_pod & 0xff) as u8),
        if src_port == 0 { 1 } else { src_port },
    )
}

thread_local! {
    static WORKER: std::cell::RefCell<Option<Worker>> =
        const { std::cell::RefCell::new(None) };
    /// Calls naming a worker other than this thread's, so the warning about
    /// them can be rate limited like every other one.
    static FOREIGN_CALLS: Cell<u64> = const { Cell::new(0) };
}

/// Reach this thread's worker. Every entry point names the worker it means and
/// worker state is thread-local, so a call naming another worker would advance
/// the wrong runtime: it is refused here rather than served.
fn with_worker<R: Copy>(worker_id: c_int, refused: R, f: impl FnOnce(&mut Worker) -> R) -> R {
    let (result, foreign) = WORKER.with(|slot| {
        let mut slot = slot.borrow_mut();
        match slot.as_mut() {
            Some(w) if w.id == worker_id => (f(w), None),
            Some(w) => (refused, Some(w.id)),
            None => (refused, None),
        }
    });
    if let Some(mine) = foreign {
        let n = FOREIGN_CALLS.with(|c| {
            c.set(c.get() + 1);
            c.get()
        });
        if rate_limited(n) {
            eprintln!(
                "[l7_linkerd] call named worker {worker_id} on worker {mine}'s thread \
                 — refused (total {n})"
            );
        }
    }
    result
}

/// Let the runtime run whatever is ready without blocking the worker loop.
fn pump(rt: &tokio::runtime::Runtime) {
    rt.block_on(async { tokio::task::yield_now().await });
}

/// Move one endpoint's output onto the DPUmesh connection that carries it, and
/// give back custody for what it has finished reading. Both endpoints publish
/// on the request connection; `backend` is what picks the direction it travels
/// — see `Session`. `budget` is the step's remaining byte allowance.
fn pump_side(
    worker_id: c_int,
    side: &mut Side,
    out_conn: Option<u64>,
    backend: i32,
    budget: &mut usize,
    counters: &mut Counters,
) -> Result<bool, ()> {
    let mut did = false;
    let has_rx = {
        let Some(handle) = side.handle.as_ref() else {
            return Ok(false);
        };
        if let Some(out) = out_conn {
            let want = TX_DRAIN_MAX.min(*budget);
            let tx = if want == 0 {
                Vec::new()
            } else {
                handle.take_tx(want)
            };
            if !tx.is_empty() {
                let accepted = datapath::send(worker_id, out, backend, &tx);
                if accepted < 0 {
                    counters.send_errors += 1;
                    return Err(());
                }
                let accepted = accepted as usize;
                if accepted > tx.len() {
                    // The datapath cannot have taken more than it was offered:
                    // the surplus names bytes that were never produced, and
                    // the stream would be short by exactly that much.
                    counters.send_errors += 1;
                    return Err(());
                }
                if accepted < tx.len() {
                    // Only the suffix goes back, in order and exactly once, so
                    // no byte is offered to the datapath twice.
                    handle.untake_tx(&tx[accepted..]);
                    counters.send_retries += 1;
                }
                if accepted > 0 {
                    *budget -= accepted;
                    if backend == BACKEND_ORIGIN {
                        counters.bytes_to_origin += accepted as u64;
                    } else {
                        counters.bytes_to_backend += accepted as u64;
                    }
                    did = true;
                }
            }
        }
        handle.has_rx()
    };
    // The whole pushed queue is consumed, so every extent it covered has been
    // read out of staging and can go back to its sender.
    if !has_rx && !side.outstanding.is_empty() && side.release_outstanding(worker_id, counters) > 0
    {
        did = true;
    }
    Ok(did)
}

impl Worker {
    fn collect_registrations(&mut self) -> bool {
        let mut did = false;
        while let Ok((slot, handle)) = self.registrations.try_recv() {
            if let Some(key) = self.pending.remove(&slot) {
                if let Some(s) = self.sessions.get_mut(&key) {
                    s.client.handle = Some(handle);
                    did = true;
                }
            }
        }
        did
    }

    /// Publish what the stack wrote, and return custody for what it has read.
    fn drain(&mut self) -> bool {
        let Worker {
            id,
            sessions,
            order,
            drain_next,
            counters,
            ..
        } = self;
        let worker_id = *id;
        let n = order.len();
        if n == 0 {
            return false;
        }
        let mut did = false;
        let mut budget = STEP_DRAIN_MAX;
        let mut failed = Vec::new();
        for _ in 0..n.min(STEP_SESSIONS_MAX) {
            let key = order[*drain_next % n];
            *drain_next = drain_next.wrapping_add(1);
            let Some(s) = sessions.get_mut(&key) else {
                continue;
            };
            // Both endpoints publish on the request connection: what the proxy
            // writes to the backend travels onward on it, what it writes to the
            // client returns along it.
            let request = s.client.conn;
            let client = pump_side(
                worker_id,
                &mut s.client,
                request,
                BACKEND_ORIGIN,
                &mut budget,
                counters,
            );
            let backend = pump_side(
                worker_id,
                &mut s.backend,
                request,
                BACKEND_ANY,
                &mut budget,
                counters,
            );
            match (client, backend) {
                (Ok(a), Ok(b)) => did |= a | b,
                _ => failed.push(key),
            }
            if budget == 0 {
                break;
            }
        }
        for key in failed {
            self.close_session(key);
        }
        did
    }

    fn close_session(&mut self, key: u64) {
        let Some(mut s) = self.sessions.remove(&key) else {
            return;
        };
        self.order.retain(|&k| k != key);
        if let Some(c) = s.client.conn {
            self.by_conn.remove(&c);
        }
        if let Some(c) = s.backend.conn {
            self.by_conn.remove(&c);
        }
        self.pending.remove(&s.slot);
        let slot = s.slot;
        let addr = s.backend_addr;
        // Custody before the endpoints go: an extent still held here would
        // never be returned, and its sender's slot would never come back.
        s.client.release_outstanding(self.id, &mut self.counters);
        s.backend.release_outstanding(self.id, &mut self.counters);
        drop(s);
        // Whatever the connector never took must not outlive the session.
        let _ = dmesh_doca::backend::take(&addr);
        let _ = self.events.send(DmeshEvent::ConnClosed(slot));
        self.counters.connections_closed += 1;
        if rate_limited(self.counters.connections_closed) {
            eprintln!(
                "[l7_linkerd] worker {} session closed ({addr}): {}",
                self.id,
                self.counters.summary()
            );
        }
    }

    /// Count a refusal and say why. The data plane forwards the connection at
    /// L4 and counts the same reason, so the two logs line up. `addr` is the
    /// session's backend address where there is one — a reply that matches no
    /// session names no address, and a synthetic one would be a guess.
    fn decline(
        &mut self,
        why: Decline,
        conn: u64,
        flow: &DmeshL7Flow,
        addr: Option<SocketAddr>,
    ) -> c_int {
        self.counters.connections_declined += 1;
        if rate_limited(self.counters.connections_declined) {
            eprintln!(
                "[l7_linkerd] worker {} declined conn {conn} \
                 (dst_service={} backend_addr={} reason={}) — forwarding at L4 \
                 (total {})",
                self.id,
                flow.dst_service,
                addr.map_or_else(|| "-".to_string(), |a| a.to_string()),
                why.reason(),
                self.counters.connections_declined
            );
        }
        why.code()
    }

    /// Attach a reply to the session it belongs to. A reply is not a new
    /// session: it is the same client seen from the other end, and it opens no
    /// backend channel of its own.
    fn attach_reply(&mut self, conn: u64, flow: &DmeshL7Flow) -> c_int {
        // A reply is named by the session it belongs to, not by its own flow:
        // it carries the upstream's identifiers, and its `dst_service` is not
        // the service the session was opened for.
        let key = session_key(flow.peer_pod, flow.dst_port);
        let attached = match self.sessions.get_mut(&key) {
            None => None,
            // One reply direction at a time. A second would push another pod's
            // staging into the same endpoint, and the first reply's extents
            // would lose the connection they must be released to.
            Some(s) if s.backend.conn.is_some_and(|c| c != conn) => Some((false, s.backend_addr)),
            Some(s) => {
                s.backend.conn = Some(conn);
                s.backend.staging_set = false;
                Some((true, s.backend_addr))
            }
        };
        match attached {
            None => self.decline(Decline::UnknownReply, conn, flow, None),
            Some((false, addr)) => self.decline(Decline::SessionLimit, conn, flow, Some(addr)),
            Some((true, addr)) => {
                self.by_conn.insert(conn, key);
                self.counters.reply_connections_attached += 1;
                tracing::info!(
                    conn,
                    session = key,
                    %addr,
                    direction = "reply",
                    "dmesh session attached"
                );
                0
            }
        }
    }

    /// Open a request session, or refuse it whole. Nothing partial is left
    /// behind: the registry entry and the maps are made together and unmade
    /// together, so a failure cannot leave a published channel no session owns.
    fn open_request(&mut self, conn: u64, flow: &DmeshL7Flow) -> c_int {
        let backend_addr = service_addr(flow.dst_service);

        // One active session per service address. The registry is keyed by
        // address and hands each entry out once, so a second session would
        // overwrite the first one's channel and the first would then be talking
        // to a connector that never took it. Temporary PoC guard: the fix is a
        // per-connection channel, which `CONTRACT.md` §10 states as the target.
        if self
            .sessions
            .values()
            .any(|s| s.backend_addr == backend_addr)
            || dmesh_doca::backend::contains(&backend_addr)
        {
            return self.decline(Decline::SessionLimit, conn, flow, Some(backend_addr));
        }

        let workload = {
            let bytes = &flow.workload;
            let end = bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len());
            // `c_char` is unsigned on aarch64 and signed elsewhere; the cast is
            // what lets one source compile for both.
            #[allow(clippy::unnecessary_cast)]
            let raw: Vec<u8> = bytes[..end].iter().map(|&c| c as u8).collect();
            String::from_utf8_lossy(&raw).into_owned()
        };
        let src = pod_addr(flow.src_pod, flow.src_port);
        let dst = service_addr_v4(flow.dst_service);

        let slot_idx = self.next_slot;
        self.next_slot += 1;

        // The endpoint the connector reaches the backend through. Publishing it
        // is what keeps the proxy off a TCP dial: the bytes it writes here are
        // carried by this session's DPUmesh connection instead.
        let (backend_io, backend_handle) = dmesh_doca::dmesh_io_pair(backend_addr);
        let session = Session {
            slot: slot_idx,
            client: Side {
                conn: Some(conn),
                ..Side::default()
            },
            backend: Side {
                handle: Some(backend_handle),
                ..Side::default()
            },
            backend_addr,
        };
        dmesh_doca::backend::publish(backend_addr, backend_io);
        self.sessions.insert(conn, session);
        self.order.push(conn);
        self.by_conn.insert(conn, conn);
        self.pending.insert(slot_idx, conn);

        let ready = DmeshEvent::ConnReady(
            slot_idx,
            FlowId {
                src,
                dst,
                workload,
                is_backend: false,
            },
        );
        if self.events.send(ready).is_err() {
            eprintln!("[l7_linkerd] worker {}: acceptor gone", self.id);
            // Unmakes the registry entry, the maps and any custody held.
            self.close_session(conn);
            return self.decline(Decline::Error, conn, flow, Some(backend_addr));
        }
        self.counters.connections_opened += 1;
        tracing::info!(
            conn,
            service = flow.dst_service,
            addr = %backend_addr,
            direction = "request",
            "dmesh session opened"
        );
        // Let the acceptor build the endpoint before the first segment lands.
        pump(&self.rt);
        self.collect_registrations();
        0
    }
}

/// Build this worker's proxy. Everything the proxy needs comes from the
/// environment, exactly as it does for the standalone binary.
#[cfg(not(test))]
fn build_worker(worker_id: c_int) -> Result<Option<Worker>, String> {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| format!("runtime: {e}"))?;

    // One proxy per ARM worker would need one set of listen ports per worker.
    // Until that is settled (CONTRACT.md open decision 6), a single worker
    // carries the proxy and the others forward at L4.
    let only: c_int = std::env::var("DPUMESH_L7_LINKERD_WORKER")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    if worker_id != only {
        return Ok(None);
    }

    // Before parsing: the parser reports which variable it rejected through
    // tracing, and without a subscriber that reason is lost.
    let trace = linkerd_app::trace::Settings::from_env()
        .init()
        .map_err(|e| format!("trace: {e}"))?;

    let config = linkerd_app::Config::try_from_env().map_err(|e| format!("config: {e}"))?;

    let (events_tx, events_rx) = mpsc::unbounded_channel::<DmeshEvent>();
    let (registrar, registrations) = mpsc::unbounded_channel::<Registration>();

    // Everything from here needs the runtime's reactor: the acceptor and the
    // proxy's own tasks are spawned onto it.
    let drain = rt.block_on(async {
        let (shutdown_tx, _shutdown_rx) = mpsc::unbounded_channel();
        let metrics = linkerd_metrics::prom::Registry::default();
        let app = config
            .build(
                linkerd_app::BindTcp::with_orig_dst(),
                linkerd_app::BindTcp::dual_with_orig_dst(),
                linkerd_app::BindTcp::default(),
                shutdown_tx,
                trace,
                metrics,
            )
            .await
            .map_err(|e| format!("app: {e}"))?;
        app.spawn_dmesh(events_rx, registrar);
        // Starts the discovery, policy and admin tasks the outbound stack
        // depends on. The returned signal shuts the proxy down when dropped.
        Ok::<_, String>(app.spawn())
    })?;
    let _ = events_tx.send(DmeshEvent::InfraReady);

    Ok(Some(Worker {
        id: worker_id,
        _drain: Box::new(drain),
        rt,
        events: events_tx,
        registrations,
        sessions: HashMap::new(),
        by_conn: HashMap::new(),
        order: Vec::new(),
        drain_next: 0,
        next_slot: 0,
        pending: HashMap::new(),
        counters: Counters::default(),
    }))
}

// ---- the contract ----

/// # Safety
/// Called once per ARM worker thread, on that thread.
#[cfg(not(test))]
#[no_mangle]
pub unsafe extern "C" fn l7_worker_attach(worker_id: c_int) -> c_int {
    linkerd_rustls::install_default_provider();
    match build_worker(worker_id) {
        Ok(Some(w)) => {
            let summary = w.counters.summary();
            WORKER.with(|slot| *slot.borrow_mut() = Some(w));
            eprintln!("[l7_linkerd] worker {worker_id} attached: proxy running; {summary}");
            0
        }
        Ok(None) => {
            eprintln!("[l7_linkerd] worker {worker_id} attached: forwards at L4");
            0
        }
        Err(e) => {
            eprintln!("[l7_linkerd] worker {worker_id} attach failed: {e}");
            -1
        }
    }
}

/// # Safety
/// Called on the worker's own thread, never re-entrantly.
#[no_mangle]
pub unsafe extern "C" fn l7_worker_step(worker_id: c_int) -> c_int {
    with_worker(worker_id, 0, |w| {
        pump(&w.rt);
        let mut did = w.collect_registrations();
        did |= w.drain();
        did as c_int
    })
}

/// # Safety
/// `flow` must point to a valid `struct dmesh_l7_flow`.
#[no_mangle]
pub unsafe extern "C" fn l7_conn_open(
    worker_id: c_int,
    conn: u64,
    flow: *const DmeshL7Flow,
) -> c_int {
    if flow.is_null() {
        return DECLINE_ERROR;
    }
    let flow = &*flow;
    if flow.mode != MODE_OPAQUE && flow.mode != MODE_FULL {
        return DECLINE_MODE; // `decision` has no payload for the proxy to carry
    }
    with_worker(worker_id, DECLINE_NOT_ATTACHED, |w| {
        // The handle carries the pod in one byte. A pod outside that range
        // would alias another one's sessions, so it is refused before anything
        // is keyed on it.
        let key_pod = if flow.is_reply != 0 {
            flow.peer_pod
        } else {
            flow.src_pod
        };
        if !(0..=0xff).contains(&key_pod) {
            // Nothing has been keyed yet, so there is no session address to name.
            return w.decline(Decline::Error, conn, flow, None);
        }
        if flow.is_reply != 0 {
            w.attach_reply(conn, flow)
        } else {
            w.open_request(conn, flow)
        }
    })
}

/// # Safety
/// `base` must point to the pod staging region for the lifetime of the segment.
#[no_mangle]
pub unsafe extern "C" fn l7_conn_segment(
    worker_id: c_int,
    conn: u64,
    base: *const u8,
    pos: u32,
    len: u32,
) -> c_int {
    if base.is_null() || len == 0 {
        return 0;
    }
    if len > c_int::MAX as u32 {
        return -1; // not a length the return value can report
    }
    with_worker(worker_id, -1, |w| {
        let Some(&key) = w.by_conn.get(&conn) else {
            return -1;
        };
        // The client endpoint arrives from the acceptor a step later.
        if w.sessions
            .get(&key)
            .is_some_and(|s| s.client.handle.is_none())
        {
            pump(&w.rt);
            w.collect_registrations();
        }
        let Some(s) = w.sessions.get_mut(&key) else {
            return -1;
        };
        // Bytes from the client feed the client endpoint; bytes from the
        // backend feed the backend endpoint.
        let side = if s.client.conn == Some(conn) {
            &mut s.client
        } else {
            &mut s.backend
        };
        let Some(handle) = side.handle.as_ref() else {
            return 0;
        };
        if !side.staging_set {
            handle.set_staging(base as usize, STAGING_SPAN);
            side.staging_set = true;
        }
        handle.push_segment(pos, len);
        side.outstanding.push((pos, len));
        w.counters.bytes_into_linkerd += len as u64;
        len as c_int
    })
}

/// # Safety
/// Called on the worker's own thread, for a connection it opened.
#[no_mangle]
pub unsafe extern "C" fn l7_conn_eof(worker_id: c_int, conn: u64) {
    with_worker(worker_id, (), |w| {
        let Some(&key) = w.by_conn.get(&conn) else {
            return;
        };
        if let Some(s) = w.sessions.get_mut(&key) {
            let side = if s.client.conn == Some(conn) {
                &s.client
            } else {
                &s.backend
            };
            if let Some(h) = side.handle.as_ref() {
                h.close_rx();
            }
        }
    });
}

/// # Safety
/// Called on the worker's own thread. Every reference into this connection's
/// staging is dropped before it returns.
#[no_mangle]
pub unsafe extern "C" fn l7_conn_close(worker_id: c_int, conn: u64) {
    with_worker(worker_id, (), |w| {
        // A session ends with its client connection; a reply closing only
        // ends that direction.
        match w.by_conn.get(&conn).copied() {
            Some(key) if key == conn => w.close_session(key),
            Some(key) => {
                w.by_conn.remove(&conn);
                let Worker {
                    id,
                    sessions,
                    counters,
                    ..
                } = w;
                if let Some(s) = sessions.get_mut(&key) {
                    s.backend.detach(*id, counters);
                }
            }
            None => {}
        }
    });
}

/// # Safety
/// Called once per ARM worker thread, on that thread, after its last step.
#[no_mangle]
pub unsafe extern "C" fn l7_worker_detach(worker_id: c_int) {
    // Sessions are closed rather than dropped: the extents they hold are the
    // data plane's, and dropping them would strand the senders' slots.
    let mine = with_worker(worker_id, false, |w| {
        for key in w.order.clone() {
            w.close_session(key);
        }
        true
    });
    // Only this thread's own worker goes. A call naming another one has already
    // been refused above, and taking the runtime here would shut down a proxy
    // that was never asked to stop.
    if !mine {
        return;
    }
    WORKER.with(|slot| {
        if let Some(w) = slot.borrow_mut().take() {
            eprintln!(
                "[l7_linkerd] worker {worker_id} detached: {} sessions left; {}",
                w.sessions.len(),
                w.counters.summary()
            );
        }
    });
}

/// `decision` mode is not the proxy's model: it answers by carrying a
/// connection, not by returning a verdict for one it never sees. The symbol is
/// exported so the datapath links either way; a negative answer leaves the
/// connection on the data plane's own path.
///
/// # Safety
/// `flow` and `out` are the data plane's, valid for the call. Neither is read
/// or written: the answer does not depend on them.
#[no_mangle]
pub unsafe extern "C" fn l7_resolve(
    _worker_id: c_int,
    _flow: *const DmeshL7Flow,
    _out: *mut DmeshL7Verdict,
) -> c_int {
    -1
}

/// Load a `decision` connection placed on its backend. The proxy answers no
/// `decision` query, so there is none to report.
///
/// # Safety
/// Called on the worker's own thread.
#[no_mangle]
pub unsafe extern "C" fn l7_report(
    _worker_id: c_int,
    _conn: u64,
    _bytes_in: u64,
    _bytes_out: u64,
    _duration_ns: u64,
    _reason: c_int,
) {
}

/// The layout `dmesh_l7.h` declares. `tests/l7_abi_contract_test.c` asserts the
/// same numbers against the C structures, so a change on either side fails a
/// build rather than corrupting a flow at run time.
#[cfg(test)]
mod abi {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn flow_layout_matches_c() {
        assert_eq!(size_of::<DmeshL7Flow>(), 92);
        assert_eq!(align_of::<DmeshL7Flow>(), 4);
        assert_eq!(offset_of!(DmeshL7Flow, src_ip), 0);
        assert_eq!(offset_of!(DmeshL7Flow, dst_ip), 4);
        assert_eq!(offset_of!(DmeshL7Flow, src_port), 8);
        assert_eq!(offset_of!(DmeshL7Flow, dst_port), 10);
        assert_eq!(offset_of!(DmeshL7Flow, src_pod), 12);
        assert_eq!(offset_of!(DmeshL7Flow, dst_service), 16);
        assert_eq!(offset_of!(DmeshL7Flow, peer_pod), 20);
        assert_eq!(offset_of!(DmeshL7Flow, mode), 24);
        assert_eq!(offset_of!(DmeshL7Flow, is_reply), 25);
        assert_eq!(offset_of!(DmeshL7Flow, workload), 26);
    }

    #[test]
    fn verdict_layout_matches_c() {
        assert_eq!(size_of::<DmeshL7Verdict>(), 8);
        assert_eq!(align_of::<DmeshL7Verdict>(), 4);
        assert_eq!(offset_of!(DmeshL7Verdict, allow), 0);
        assert_eq!(offset_of!(DmeshL7Verdict, backend_pod), 4);
    }

    #[test]
    fn constants_match_c() {
        assert_eq!(MODE_OPAQUE, 2);
        assert_eq!(MODE_FULL, 3);
        assert_eq!(BACKEND_ANY, -1);
        assert_eq!(BACKEND_ORIGIN, -2);
        assert_eq!(DECLINE_ERROR, -1);
        assert_eq!(DECLINE_NOT_ATTACHED, -2);
        assert_eq!(DECLINE_MODE, -3);
        assert_eq!(DECLINE_SESSION_LIMIT, -4);
        assert_eq!(DECLINE_UNKNOWN_REPLY, -5);
    }
}

#[cfg(test)]
mod tests {
    use super::datapath::fake;
    use super::*;
    use dmesh_doca::DmeshIo;
    use tokio::io::AsyncWriteExt;

    /// A worker with no proxy behind it. The entry points below are the real
    /// ones; only the datapath and the runtime's tenants are stand-ins.
    struct TestWorker {
        events: mpsc::UnboundedReceiver<DmeshEvent>,
        registrar: mpsc::UnboundedSender<Registration>,
    }

    fn install_worker(id: c_int) -> TestWorker {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let (events_tx, events) = mpsc::unbounded_channel();
        let (registrar, registrations) = mpsc::unbounded_channel();
        let w = Worker {
            id,
            _drain: Box::new(()),
            rt,
            events: events_tx,
            registrations,
            sessions: HashMap::new(),
            by_conn: HashMap::new(),
            order: Vec::new(),
            drain_next: 0,
            next_slot: 0,
            pending: HashMap::new(),
            counters: Counters::default(),
        };
        WORKER.with(|slot| *slot.borrow_mut() = Some(w));
        fake::reset();
        TestWorker { events, registrar }
    }

    fn with_test_worker<R>(f: impl FnOnce(&mut Worker) -> R) -> R {
        WORKER.with(|slot| f(slot.borrow_mut().as_mut().unwrap()))
    }

    /// Each test uses its own service id, because the backend registry is
    /// process-wide while `WORKER` is per thread.
    fn request_flow(service: i32, pod: i32, port: u16) -> DmeshL7Flow {
        DmeshL7Flow {
            src_ip: 0,
            dst_ip: 0,
            src_port: port,
            dst_port: port,
            src_pod: pod,
            dst_service: service,
            peer_pod: pod,
            mode: MODE_OPAQUE,
            is_reply: 0,
            workload: [0; 64],
        }
    }

    fn reply_flow(service: i32, peer_pod: i32, dst_port: u16) -> DmeshL7Flow {
        DmeshL7Flow {
            is_reply: 1,
            peer_pod,
            dst_port,
            ..request_flow(service, peer_pod, dst_port)
        }
    }

    /// Stand in for the acceptor: hand the worker the client endpoint it is
    /// waiting for, and keep the stack's side of it.
    fn register_client(tw: &TestWorker, slot: usize) -> DmeshIo {
        let (io, handle) = dmesh_doca::dmesh_io_pair("10.97.0.1:1".parse().unwrap());
        tw.registrar.send((slot, handle)).unwrap();
        with_test_worker(|w| w.collect_registrations());
        io
    }

    fn write_to(io: &mut DmeshIo, bytes: &[u8]) {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        rt.block_on(async { io.write_all(bytes).await.unwrap() });
    }

    fn sent() -> Vec<(u64, i32, Vec<u8>)> {
        fake::STATE.with(|s| std::mem::take(&mut s.borrow_mut().sent))
    }

    fn released() -> Vec<(u64, u32, u32)> {
        fake::STATE.with(|s| s.borrow_mut().released.clone())
    }

    #[test]
    fn session_key_matches_c_handle() {
        // The same vectors `tests/l7_abi_contract_test.c` puts through
        // `dmesh_l7_conn_handle()`.
        let vectors: &[(i32, u16, u64)] = &[
            (0, 0, 0x0000_0000),
            (0, 1, 0x0000_0001),
            (1, 9092, 0x0001_2384),
            (11, 40000, 0x000b_9c40),
            (127, 65535, 0x007f_ffff),
            (128, 1, 0x0080_0001),
            (255, 1, 0x00ff_0001),
            (-1, 1, 0x00ff_0001),
        ];
        for &(pod, port, want) in vectors {
            assert_eq!(session_key(pod, port), want, "pod {pod} port {port}");
        }
    }

    #[test]
    fn second_session_for_same_service_is_rejected() {
        let _tw = install_worker(0);
        let first = request_flow(21, 1, 4001);
        let second = request_flow(21, 2, 4002);
        assert_eq!(unsafe { l7_conn_open(0, 1, &first) }, 0);
        assert_eq!(
            unsafe { l7_conn_open(0, 2, &second) },
            DECLINE_SESSION_LIMIT
        );
        with_test_worker(|w| {
            assert_eq!(w.sessions.len(), 1);
            assert_eq!(w.order.len(), 1);
            assert!(!w.by_conn.contains_key(&2));
            assert_eq!(w.counters.connections_declined, 1);
        });
        // The first session's channel is still the one on offer.
        assert!(dmesh_doca::backend::contains(&service_addr(21)));
        with_test_worker(|w| w.close_session(1));
    }

    #[test]
    fn reply_attaches_to_existing_session() {
        let _tw = install_worker(0);
        let req = request_flow(22, 3, 4003);
        let key = session_key(3, 4003);
        assert_eq!(unsafe { l7_conn_open(0, key, &req) }, 0);
        let rep = reply_flow(22, 3, 4003);
        assert_eq!(unsafe { l7_conn_open(0, 777, &rep) }, 0);
        with_test_worker(|w| {
            assert_eq!(w.by_conn.get(&777), Some(&key));
            assert_eq!(w.sessions[&key].backend.conn, Some(777));
            assert_eq!(w.counters.reply_connections_attached, 1);
            assert_eq!(w.sessions.len(), 1, "a reply is not a new session");
        });
        // A second, concurrent reply direction is refused rather than swapped in.
        assert_eq!(unsafe { l7_conn_open(0, 778, &rep) }, DECLINE_SESSION_LIMIT);
        with_test_worker(|w| {
            assert_eq!(w.sessions[&key].backend.conn, Some(777));
            w.close_session(key);
        });
    }

    #[test]
    fn unknown_reply_is_rejected() {
        let _tw = install_worker(0);
        let rep = reply_flow(23, 9, 4009);
        assert_eq!(
            unsafe { l7_conn_open(0, 4242, &rep) },
            DECLINE_UNKNOWN_REPLY
        );
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert!(w.by_conn.is_empty());
        });
        assert!(!dmesh_doca::backend::contains(&service_addr(23)));
    }

    /// Open a session whose backend endpoint has `bytes` waiting to go out.
    fn session_with_backend_output(service: i32, conn: u64, bytes: &[u8]) -> DmeshIo {
        let flow = request_flow(service, 5, 5000);
        assert_eq!(unsafe { l7_conn_open(0, conn, &flow) }, 0);
        let mut io = dmesh_doca::backend::take(&service_addr(service)).unwrap();
        write_to(&mut io, bytes);
        io
    }

    #[test]
    fn full_send_does_not_requeue() {
        let _tw = install_worker(0);
        let _io = session_with_backend_output(24, 1, b"0123456789");
        assert_eq!(unsafe { l7_worker_step(0) }, 1);
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123456789".to_vec())]);
        // Nothing was put back, so a second step has nothing to publish.
        unsafe { l7_worker_step(0) };
        assert!(sent().is_empty());
        with_test_worker(|w| {
            assert_eq!(w.counters.send_retries, 0);
            assert_eq!(w.counters.bytes_to_backend, 10);
            w.close_session(1);
        });
    }

    #[test]
    fn zero_send_requeues_every_byte() {
        let _tw = install_worker(0);
        let _io = session_with_backend_output(25, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().accept = Some(0));
        unsafe { l7_worker_step(0) };
        assert!(sent().is_empty());
        fake::STATE.with(|s| s.borrow_mut().accept = None);
        unsafe { l7_worker_step(0) };
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123456789".to_vec())]);
        with_test_worker(|w| {
            assert_eq!(w.counters.send_retries, 1);
            w.close_session(1);
        });
    }

    #[test]
    fn partial_send_requeues_only_suffix() {
        let _tw = install_worker(0);
        let _io = session_with_backend_output(26, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().accept = Some(4));
        unsafe { l7_worker_step(0) };
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123".to_vec())]);
        fake::STATE.with(|s| s.borrow_mut().accept = None);
        unsafe { l7_worker_step(0) };
        // The suffix, once, in order: no byte is offered twice.
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"456789".to_vec())]);
        with_test_worker(|w| w.close_session(1));
    }

    #[test]
    fn over_accept_is_terminal() {
        let _tw = install_worker(0);
        let _io = session_with_backend_output(27, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().over_accept = true);
        unsafe { l7_worker_step(0) };
        with_test_worker(|w| {
            assert_eq!(w.counters.send_errors, 1);
            assert!(w.sessions.is_empty(), "the session is closed, not resumed");
        });
    }

    #[test]
    fn close_releases_every_outstanding_extent_once() {
        let tw = install_worker(0);
        let flow = request_flow(28, 6, 6000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let _client = register_client(&tw, 0);
        let staging = vec![7u8; 4096];
        unsafe {
            assert_eq!(l7_conn_segment(0, 1, staging.as_ptr(), 0, 16), 16);
            assert_eq!(l7_conn_segment(0, 1, staging.as_ptr(), 16, 32), 32);
        }
        with_test_worker(|w| {
            assert_eq!(w.sessions[&1].client.outstanding.len(), 2);
            assert_eq!(w.counters.bytes_into_linkerd, 48);
        });
        // Nothing read them, so close is what returns them.
        unsafe { l7_conn_close(0, 1) };
        assert_eq!(released(), vec![(1, 0, 16), (1, 16, 32)]);
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert!(w.by_conn.is_empty());
            assert!(w.order.is_empty());
            assert_eq!(w.counters.segments_released, 2);
        });
        assert!(!dmesh_doca::backend::contains(&service_addr(28)));
    }

    #[test]
    fn normal_drain_then_close_does_not_double_release() {
        let tw = install_worker(0);
        let flow = request_flow(29, 7, 7000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let mut client = register_client(&tw, 0);
        let staging = vec![7u8; 4096];
        unsafe { assert_eq!(l7_conn_segment(0, 1, staging.as_ptr(), 0, 16), 16) };

        // The stack reads the segment, so the step returns its custody.
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        rt.block_on(async {
            use tokio::io::AsyncReadExt;
            let mut buf = [0u8; 16];
            client.read_exact(&mut buf).await.unwrap();
        });
        unsafe { l7_worker_step(0) };
        assert_eq!(released(), vec![(1, 0, 16)]);

        unsafe { l7_conn_close(0, 1) };
        assert_eq!(released(), vec![(1, 0, 16)], "released once, not twice");
        with_test_worker(|w| assert_eq!(w.counters.segments_released, 1));
    }

    #[test]
    fn wrong_worker_id_is_rejected() {
        let _tw = install_worker(0);
        let flow = request_flow(30, 8, 8000);
        assert_eq!(
            unsafe { l7_conn_open(1, 1, &flow) },
            DECLINE_NOT_ATTACHED,
            "another worker's runtime is not this thread's to open on"
        );
        assert_eq!(unsafe { l7_worker_step(1) }, 0);
        assert_eq!(
            unsafe { l7_conn_segment(1, 1, [0u8; 8].as_ptr(), 0, 8) },
            -1
        );
        unsafe { l7_conn_close(1, 1) };
        unsafe { l7_worker_detach(1) };
        WORKER.with(|slot| {
            assert!(
                slot.borrow().is_some(),
                "detaching worker 1 must not take worker 0's runtime"
            )
        });
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert_eq!(w.counters.connections_opened, 0);
        });
        assert!(!dmesh_doca::backend::contains(&service_addr(30)));
    }

    #[test]
    #[allow(non_snake_case)]
    fn ConnReady_failure_rolls_back_registry_and_maps() {
        let tw = install_worker(0);
        drop(tw.events); // the acceptor is gone before the first connection
        let flow = request_flow(31, 9, 9000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, DECLINE_ERROR);
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert!(w.by_conn.is_empty());
            assert!(w.order.is_empty());
            assert!(w.pending.is_empty());
            assert_eq!(w.counters.connections_opened, 0);
        });
        assert!(
            !dmesh_doca::backend::contains(&service_addr(31)),
            "a published channel must not outlive the session that published it"
        );
    }

    #[test]
    fn detach_releases_outstanding_custody() {
        let tw = install_worker(0);
        let flow = request_flow(32, 10, 10000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let _client = register_client(&tw, 0);
        let staging = vec![7u8; 4096];
        unsafe { assert_eq!(l7_conn_segment(0, 1, staging.as_ptr(), 64, 8), 8) };
        unsafe { l7_worker_detach(0) };
        assert_eq!(released(), vec![(1, 64, 8)]);
        assert!(!dmesh_doca::backend::contains(&service_addr(32)));
        WORKER.with(|slot| assert!(slot.borrow().is_none()));
    }
}
