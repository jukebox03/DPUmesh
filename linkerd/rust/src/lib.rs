//! Embedded Linkerd outbound adapter for DPUmesh.
//!
//! DPUmesh owns DOCA, progress engines, DMA rings and worker threads. Each ARM
//! worker hosts a Tokio `current_thread` runtime and the persistent driver in
//! `dmesh_doca::runtime`. One configured worker carries Linkerd sessions.

use std::cell::Cell;
use std::collections::HashMap;
#[cfg(not(test))]
use std::ffi::c_void;
#[cfg(not(test))]
use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::os::raw::{c_char, c_int};
use std::sync::Arc;
#[cfg(not(test))]
use std::task::{Context, Poll};

use dmesh_doca::{
    BackendKey, Backends, DmeshEvent, DmeshIoHandle, FlowId, Registration, SessionMetrics,
    SessionToken, Slots,
};
use tokio::sync::mpsc;

/// Rust layout of `struct dmesh_l7_flow`.
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
/// Return output to the connection's sender.
const BACKEND_ORIGIN: i32 = -2;

/// `DMESH_L7_DECLINE_*` ABI values.
const DECLINE_ERROR: c_int = -1;
const DECLINE_NOT_ATTACHED: c_int = -2;
const DECLINE_MODE: c_int = -3;
const DECLINE_SESSION_LIMIT: c_int = -4;
const DECLINE_UNKNOWN_REPLY: c_int = -5;

/// Maximum pod staging span.
const STAGING_SPAN: usize = 64 * 1024 * 1024;

/// Per-connection output budget for one drain pass.
const TX_DRAIN_MAX: usize = 64 * 1024;

/// Reservations one connection may publish in a drain pass. A reservation is
/// one egress chunk, so this is what makes the reservation path's per-pass
/// volume the same as the copy path's `dmesh_l7_send` cap.
const TX_RESERVATIONS_MAX: usize = 4;

/// Aggregate output and session budgets for one drain pass.
const DRAIN_MAX: usize = 256 * 1024;
const DRAIN_SESSIONS_MAX: usize = 64;

/// Port used in synthetic service addresses.
const SERVICE_PORT: u16 = 9092;

/// DPUmesh ABI calls with a recording test implementation.
mod datapath {
    #[cfg(test)]
    pub use fake::{release, send, tx_publish};

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
        fn dmesh_l7_tx_reserve(worker_id: c_int, conn: u64, cap: *mut u32) -> *mut u8;
        fn dmesh_l7_tx_commit(worker_id: c_int, conn: u64, backend_pod: i32, len: u32) -> c_int;
    }

    #[cfg(not(test))]
    pub fn send(worker_id: c_int, conn: u64, backend_pod: i32, buf: &[u8]) -> c_int {
        unsafe { dmesh_l7_send(worker_id, conn, backend_pod, buf.as_ptr(), buf.len()) }
    }

    #[cfg(not(test))]
    pub fn release(worker_id: c_int, conn: u64, pos: u32, len: u32) {
        unsafe { dmesh_l7_release(worker_id, conn, pos, len) }
    }

    /// Write output straight into the connection's egress chunk.
    ///
    /// `fill` is handed the reservation and answers how many bytes it wrote;
    /// the reservation is always committed, with length 0 cancelling it.
    /// `None` means the datapath had no chunk to lend.
    #[cfg(not(test))]
    pub fn tx_publish(
        worker_id: c_int,
        conn: u64,
        backend_pod: i32,
        fill: impl FnOnce(&mut [u8]) -> usize,
    ) -> Option<c_int> {
        let mut cap: u32 = 0;
        let base = unsafe { dmesh_l7_tx_reserve(worker_id, conn, &mut cap) };
        if base.is_null() || cap == 0 {
            return None;
        }
        // SAFETY: the datapath lends `cap` writable bytes of its egress arena
        // until the commit below, and this thread owns the reservation.
        let reservation = unsafe { std::slice::from_raw_parts_mut(base, cap as usize) };
        let len = fill(reservation).min(cap as usize) as u32;
        Some(unsafe { dmesh_l7_tx_commit(worker_id, conn, backend_pod, len) })
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
            /// Refuse the next reservation, as an exhausted arena would.
            pub no_chunk: bool,
            /// Reservation size. The datapath lends one arena chunk.
            pub chunk: usize,
            /// Reservations cancelled with a zero-length commit.
            pub cancels: usize,
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

        /// Lend a reservation, then answer as the C commit would.
        pub fn tx_publish(
            _worker_id: c_int,
            conn: u64,
            backend_pod: i32,
            fill: impl FnOnce(&mut [u8]) -> usize,
        ) -> Option<c_int> {
            let cap = STATE.with(|s| {
                let s = s.borrow();
                if s.no_chunk {
                    return 0;
                }
                if s.chunk == 0 {
                    16 * 1024
                } else {
                    s.chunk
                }
            });
            if cap == 0 {
                return None;
            }
            let mut reservation = vec![0u8; cap];
            let len = fill(&mut reservation).min(cap);
            Some(STATE.with(|s| {
                let mut s = s.borrow_mut();
                if s.fail {
                    return -1;
                }
                if len == 0 {
                    s.cancels += 1;
                    return 0;
                }
                if s.over_accept {
                    return len as c_int + 1;
                }
                if s.accept.unwrap_or(len) < len {
                    // The datapath publishes a whole reservation or none of it.
                    return 0;
                }
                s.sent
                    .push((conn, backend_pod, reservation[..len].to_vec()));
                len as c_int
            }))
        }
    }
}

#[cfg(not(test))]
extern "C" {
    fn dmesh_l7_driver_notification_fds(
        driver: *mut c_void,
        completion_fd: *mut c_int,
        dma_fd: *mut c_int,
        wake_fd: *mut c_int,
    ) -> c_int;
    fn dmesh_l7_driver_arm(driver: *mut c_void) -> c_int;
    fn dmesh_l7_driver_drain(driver: *mut c_void, budget: c_int) -> c_int;
    fn dmesh_l7_driver_clear_notifications(driver: *mut c_void) -> c_int;
    fn dmesh_l7_driver_maintenance(driver: *mut c_void) -> c_int;
    fn dmesh_l7_driver_stopped(driver: *mut c_void) -> c_int;
    fn dmesh_l7_driver_ready(driver: *mut c_void);
    fn dmesh_l7_driver_failed(driver: *mut c_void);
}

#[cfg(not(test))]
struct ExternalBackend {
    worker_id: c_int,
    driver: *mut c_void,
}

#[cfg(not(test))]
fn driver_result(code: c_int, operation: &'static str) -> io::Result<c_int> {
    if code < 0 {
        Err(io::Error::other(format!("{operation} failed ({code})")))
    } else {
        Ok(code)
    }
}

#[cfg(not(test))]
impl dmesh_doca::runtime::RuntimeBackend for ExternalBackend {
    fn notification_fds(&mut self) -> io::Result<dmesh_doca::runtime::NotificationFds> {
        let mut completion = -1;
        let mut dma = -1;
        let mut wake = -1;
        driver_result(
            unsafe {
                dmesh_l7_driver_notification_fds(self.driver, &mut completion, &mut dma, &mut wake)
            },
            "notification_fds",
        )?;
        Ok(dmesh_doca::runtime::NotificationFds {
            completion,
            dma: (dma >= 0).then_some(dma),
            wake,
        })
    }

    fn arm(&mut self) -> io::Result<()> {
        driver_result(unsafe { dmesh_l7_driver_arm(self.driver) }, "arm").map(|_| ())
    }

    fn drain(&mut self, budget: usize) -> io::Result<dmesh_doca::runtime::Progress> {
        let budget = c_int::try_from(budget).unwrap_or(c_int::MAX);
        let code = driver_result(
            unsafe { dmesh_l7_driver_drain(self.driver, budget) },
            "drain",
        )?;
        let linkerd = with_worker(self.worker_id, false, |worker| {
            worker.collect_registrations() | worker.drain()
        });
        if linkerd || code == 2 {
            Ok(dmesh_doca::runtime::Progress::Progressed)
        } else if code == 1 {
            Ok(dmesh_doca::runtime::Progress::Pending)
        } else {
            Ok(dmesh_doca::runtime::Progress::Idle)
        }
    }

    fn clear_notifications(&mut self) -> io::Result<()> {
        driver_result(
            unsafe { dmesh_l7_driver_clear_notifications(self.driver) },
            "clear_notifications",
        )
        .map(|_| ())
    }

    fn maintenance(&mut self) -> io::Result<()> {
        driver_result(
            unsafe { dmesh_l7_driver_maintenance(self.driver) },
            "maintenance",
        )
        .map(|_| ())
    }

    fn poll_internal(&mut self, cx: &mut Context<'_>) -> Poll<()> {
        with_worker(self.worker_id, Poll::Pending, |worker| {
            worker.poll_internal(cx)
        })
    }

    fn stopped(&self) -> bool {
        unsafe { dmesh_l7_driver_stopped(self.driver) != 0 }
    }

    fn ready(&mut self) {
        unsafe { dmesh_l7_driver_ready(self.driver) }
    }

    fn failed(&mut self) {
        unsafe { dmesh_l7_driver_failed(self.driver) }
    }
}

/// Per-worker adapter counters.
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
    registrations_orphaned: u64,
}

impl Counters {
    fn summary(&self) -> String {
        format!(
            "opened={} closed={} declined={} replies={} into_linkerd={} \
             to_backend={} to_origin={} released={} retries={} errors={} orphans={}",
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
            self.registrations_orphaned,
        )
    }
}

/// Log the first event and every 4096th event.
fn rate_limited(count: u64) -> bool {
    count == 1 || count.is_multiple_of(4096)
}

/// Adapter decline categories.
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

/// One DPUmesh direction and its `DmeshIo` handle.
#[derive(Default)]
struct Side {
    conn: Option<u64>,
    handle: Option<DmeshIoHandle>,
    staging_set: bool,
    /// Extents handed to Linkerd and not released.
    outstanding: Vec<(u32, u32)>,
}

impl Side {
    /// Release all outstanding staging extents.
    fn release_outstanding(&mut self, worker_id: c_int, counters: &mut Counters) -> usize {
        let Some(conn) = self.conn else {
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

    /// Abort the endpoint, release its staging custody, and detach it.
    fn detach(&mut self, worker_id: c_int, counters: &mut Counters, metrics: &SessionMetrics) {
        // The endpoint must stop referring to queued DMA segments before their
        // custody is returned to DPUmesh.
        if let Some(handle) = self.handle.as_ref() {
            handle.abort();
            metrics.endpoints_aborted.inc();
        }
        self.release_outstanding(worker_id, counters);
        self.conn = None;
        self.staging_set = false;
    }
}

/// A request connection and its Linkerd backend endpoint.
struct Session {
    /// Names this session to the acceptor for its whole lifetime.
    token: SessionToken,
    /// Linkerd's client-facing endpoint.
    client: Side,
    /// Linkerd's backend-facing endpoint.
    backend: Side,
    backend_addr: SocketAddr,
}

impl Session {
    fn backend_key(&self) -> BackendKey {
        BackendKey::new(self.backend_addr, self.token)
    }
}

struct Worker {
    id: c_int,
    /// Proxy lifetime guard.
    _drain: Box<dyn std::any::Any>,
    events: mpsc::UnboundedSender<DmeshEvent>,
    registrations: mpsc::UnboundedReceiver<Registration>,
    /// Sessions keyed by request connection handle.
    sessions: HashMap<u64, Session>,
    /// Connection handle to request session.
    by_conn: HashMap<u64, u64>,
    /// Fair drain order and cursor.
    order: Vec<u64>,
    drain_next: usize,
    /// Session tokens, and the sessions awaiting their client endpoint.
    slots: Slots,
    pending: HashMap<SessionToken, u64>,
    /// This worker's backend channels; the connector takes them from here.
    backends: Arc<Backends>,
    metrics: Arc<SessionMetrics>,
    /// Copy output into the egress arena rather than through a temporary Vec.
    tx_reserve: bool,
    counters: Counters,
}

/// DPUmesh connection handle from pod and port.
fn session_key(pod: i32, port: u16) -> u64 {
    ((pod as u8 as u64) << 16) | port as u64
}

/// Synthetic socket address for a DPUmesh service.
fn service_addr_v4(dst_service: i32) -> SocketAddrV4 {
    SocketAddrV4::new(
        Ipv4Addr::new(10, 96, 0, (dst_service & 0xff) as u8),
        SERVICE_PORT,
    )
}

/// Backend-registry address for a service.
fn service_addr(dst_service: i32) -> SocketAddr {
    SocketAddr::V4(service_addr_v4(dst_service))
}

/// Synthetic peer address for a pod.
fn pod_addr(src_pod: i32, src_port: u16) -> SocketAddrV4 {
    SocketAddrV4::new(
        Ipv4Addr::new(10, 97, 0, (src_pod & 0xff) as u8),
        if src_port == 0 { 1 } else { src_port },
    )
}

thread_local! {
    static WORKER: std::cell::RefCell<Option<Worker>> =
        const { std::cell::RefCell::new(None) };
    /// Count calls naming another worker.
    static FOREIGN_CALLS: Cell<u64> = const { Cell::new(0) };
}

/// Access the worker bound to the current thread.
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

/// Copy queued output straight into the egress arena.
///
/// One copy: from the endpoint's queue into the chunk the datapath will DMA.
/// A reservation the datapath refuses to publish is cancelled and the bytes
/// stay queued, so nothing is offered twice and nothing is lost.
fn publish_reserved(
    worker_id: c_int,
    handle: &DmeshIoHandle,
    out: u64,
    backend: i32,
    want: usize,
    counters: &mut Counters,
) -> Result<Option<usize>, ()> {
    let mut copied = 0usize;
    let Some(rc) = datapath::tx_publish(worker_id, out, backend, |chunk| {
        let room = chunk.len().min(want);
        copied = handle.copy_tx_into(&mut chunk[..room]);
        copied
    }) else {
        // No chunk to lend: the arena is dry. Retry on a later pass.
        return Ok(None);
    };
    if rc < 0 || rc as usize > copied {
        counters.send_errors += 1;
        return Err(());
    }
    let accepted = rc as usize;
    if accepted == 0 {
        if copied > 0 {
            counters.send_retries += 1;
        }
        return Ok(Some(0));
    }
    handle.consume_tx(accepted);
    if accepted < copied {
        counters.send_retries += 1;
    }
    Ok(Some(accepted))
}

/// Copy queued output through a temporary buffer and hand it to the datapath.
///
/// The compatibility path: it exists so the reservation path can be compared
/// against it on hardware, and so an arena that lends no chunk is not a stall.
fn publish_copied(
    worker_id: c_int,
    handle: &DmeshIoHandle,
    out: u64,
    backend: i32,
    want: usize,
    counters: &mut Counters,
) -> Result<usize, ()> {
    let tx = handle.take_tx(want);
    if tx.is_empty() {
        return Ok(0);
    }
    let accepted = datapath::send(worker_id, out, backend, &tx);
    if accepted < 0 {
        counters.send_errors += 1;
        return Err(());
    }
    let accepted = accepted as usize;
    if accepted > tx.len() {
        counters.send_errors += 1;
        return Err(());
    }
    if accepted < tx.len() {
        handle.untake_tx(&tx[accepted..]);
        counters.send_retries += 1;
    }
    Ok(accepted)
}

/// Publish endpoint output and release fully consumed input.
fn pump_side(
    worker_id: c_int,
    side: &mut Side,
    out_conn: Option<u64>,
    backend: i32,
    budget: &mut usize,
    reserve: bool,
    counters: &mut Counters,
) -> Result<bool, ()> {
    let mut did = false;
    let has_rx = {
        let Some(handle) = side.handle.as_ref() else {
            return Ok(false);
        };
        if let Some(out) = out_conn {
            let want = TX_DRAIN_MAX.min(*budget);
            let accepted = if want == 0 || handle.tx_len() == 0 {
                0
            } else if reserve {
                let mut total = 0;
                for _ in 0..TX_RESERVATIONS_MAX {
                    if total == want || handle.tx_len() == 0 {
                        break;
                    }
                    // `None` is an arena with no chunk to lend; the copy path
                    // needs one too, so the bytes wait for the next pass
                    // either way. `Some(0)` is a refused publication.
                    match publish_reserved(worker_id, handle, out, backend, want - total, counters)?
                    {
                        Some(n) if n > 0 => total += n,
                        _ => break,
                    }
                }
                total
            } else {
                publish_copied(worker_id, handle, out, backend, want, counters)?
            };
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
        handle.has_rx()
    };
    // Release a fully consumed input queue.
    if !has_rx && !side.outstanding.is_empty() && side.release_outstanding(worker_id, counters) > 0
    {
        did = true;
    }
    Ok(did)
}

impl Worker {
    #[cfg(not(test))]
    fn poll_internal(&mut self, cx: &mut Context<'_>) -> Poll<()> {
        for session in self.sessions.values() {
            for side in [&session.client, &session.backend] {
                if side.handle.as_ref().is_some_and(|handle| {
                    handle.tx_finished() || handle.poll_tx_ready(cx).is_ready()
                }) {
                    return Poll::Ready(());
                }
            }
        }
        Poll::Pending
    }

    fn collect_registrations(&mut self) -> bool {
        let mut did = false;
        while let Ok(Registration { token, handle }) = self.registrations.try_recv() {
            // The token names one generation of one slot. A registration for
            // any other is an endpoint whose session is gone.
            let bound = self
                .pending
                .remove(&token)
                .and_then(|key| self.sessions.get_mut(&key))
                .filter(|session| session.token == token);
            if let Some(session) = bound {
                session.client.handle = Some(handle);
                did = true;
                continue;
            }
            // ConnReady and ConnClosed may already be queued when the
            // acceptor registers this endpoint. Do not leave its task waiting
            // on an endpoint whose session is gone.
            handle.abort();
            self.counters.registrations_orphaned += 1;
            self.metrics.registrations_orphaned.inc();
            self.metrics.endpoints_aborted.inc();
        }
        self.metrics
            .registrations_pending
            .set(self.pending.len() as i64);
        did
    }

    /// Publish what the stack wrote, and return custody for what it has read.
    fn drain(&mut self) -> bool {
        let Worker {
            id,
            sessions,
            order,
            drain_next,
            tx_reserve,
            counters,
            ..
        } = self;
        let worker_id = *id;
        let reserve = *tx_reserve;
        let n = order.len();
        if n == 0 {
            return false;
        }
        let mut did = false;
        let mut budget = DRAIN_MAX;
        let mut failed = Vec::new();
        for _ in 0..n.min(DRAIN_SESSIONS_MAX) {
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
                reserve,
                counters,
            );
            let backend = pump_side(
                worker_id,
                &mut s.backend,
                request,
                BACKEND_ANY,
                &mut budget,
                reserve,
                counters,
            );
            let endpoint_finished = [&s.client, &s.backend]
                .iter()
                .any(|side| side.handle.as_ref().is_some_and(DmeshIoHandle::tx_finished));
            match (client, backend, endpoint_finished) {
                (Ok(a), Ok(b), false) => did |= a | b,
                _ => failed.push(key),
            }
            if budget == 0 {
                break;
            }
        }
        for key in failed {
            self.close_session(key);
            did = true;
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
        let token = s.token;
        self.pending.remove(&token);
        let addr = s.backend_addr;
        // Withdraw this session's channel before the next generation is
        // admitted, so a connector cannot take a closed session's endpoint.
        self.backends.remove(&s.backend_key());
        s.client.detach(self.id, &mut self.counters, &self.metrics);
        s.backend.detach(self.id, &mut self.counters, &self.metrics);
        drop(s);
        let _ = self.events.send(DmeshEvent::ConnClosed(token));
        self.slots.release(token);
        self.counters.connections_closed += 1;
        self.metrics.sessions_closed.inc();
        self.metrics.sessions_active.set(self.sessions.len() as i64);
        self.metrics
            .registrations_pending
            .set(self.pending.len() as i64);
        self.metrics.slots_retired.set(self.slots.retired() as i64);
        if rate_limited(self.counters.connections_closed) {
            let (acquired, contended) = dmesh_doca::lock_stats();
            let (registry_acquired, registry_contended) = self.backends.lock_stats();
            eprintln!(
                "[l7_linkerd] worker {} session {token} closed ({addr}): {} \
                 endpoint_lock={acquired}/{contended} \
                 registry_lock={registry_acquired}/{registry_contended}",
                self.id,
                self.counters.summary()
            );
        }
    }

    /// Count and log an adapter decline.
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

    /// Attach a reply direction to its request session.
    fn attach_reply(&mut self, conn: u64, flow: &DmeshL7Flow) -> c_int {
        // Replies identify the request by peer pod and destination port.
        let key = session_key(flow.peer_pod, flow.dst_port);
        let attached = match self.sessions.get_mut(&key) {
            None => None,
            // One reply direction per session.
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

    /// Open one request session and publish its backend endpoint.
    fn open_request(&mut self, conn: u64, flow: &DmeshL7Flow) -> c_int {
        let backend_addr = service_addr(flow.dst_service);

        let workload = {
            let bytes = &flow.workload;
            let end = bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len());
            // Normalize the platform `c_char` representation.
            #[allow(clippy::unnecessary_cast)]
            let raw: Vec<u8> = bytes[..end].iter().map(|&c| c as u8).collect();
            String::from_utf8_lossy(&raw).into_owned()
        };
        let src = pod_addr(flow.src_pod, flow.src_port);
        let dst = service_addr_v4(flow.dst_service);

        let Some(token) = self.slots.alloc() else {
            eprintln!("[l7_linkerd] worker {}: no session slot left", self.id);
            return self.decline(Decline::Error, conn, flow, Some(backend_addr));
        };

        // Publish the DPUmesh backend endpoint for the Linkerd connector.
        let (backend_io, backend_handle) = dmesh_doca::dmesh_io_pair(backend_addr);
        let session = Session {
            token,
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
        if let Err(error) = self.backends.publish(session.backend_key(), backend_io) {
            eprintln!(
                "[l7_linkerd] worker {}: backend channel refused: {error}",
                self.id
            );
            self.slots.release(token);
            return self.decline(Decline::SessionLimit, conn, flow, Some(backend_addr));
        }
        self.sessions.insert(conn, session);
        self.order.push(conn);
        self.by_conn.insert(conn, conn);
        self.pending.insert(token, conn);

        let ready = DmeshEvent::ConnReady(
            token,
            FlowId {
                src,
                dst,
                workload,
                is_backend: false,
            },
        );
        if self.events.send(ready).is_err() {
            eprintln!("[l7_linkerd] worker {}: acceptor gone", self.id);
            self.close_session(conn);
            return self.decline(Decline::Error, conn, flow, Some(backend_addr));
        }
        self.counters.connections_opened += 1;
        self.metrics.sessions_opened.inc();
        self.metrics.sessions_active.set(self.sessions.len() as i64);
        self.metrics
            .registrations_pending
            .set(self.pending.len() as i64);
        tracing::info!(
            conn,
            session = %token,
            service = flow.dst_service,
            addr = %backend_addr,
            direction = "request",
            "dmesh session opened"
        );
        0
    }
}

/// Build the configured worker's Linkerd proxy.
#[cfg(not(test))]
async fn build_worker(worker_id: c_int) -> Result<Option<Worker>, String> {
    // One worker owns Linkerd session state.
    let only: c_int = std::env::var("DPUMESH_L7_LINKERD_WORKER")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    if worker_id != only {
        return Ok(None);
    }

    linkerd_rustls::install_default_provider();

    // Initialize tracing before parsing Linkerd settings.
    let trace = linkerd_app::trace::Settings::from_env()
        .init()
        .map_err(|e| format!("trace: {e}"))?;

    let config = linkerd_app::Config::try_from_env().map_err(|e| format!("config: {e}"))?;

    let (events_tx, events_rx) = mpsc::unbounded_channel::<DmeshEvent>();
    let (registrar, registrations) = mpsc::unbounded_channel::<Registration>();

    // Build and spawn Linkerd on the current runtime.
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
    // The registry and counters this worker's connector uses.
    let backends = app.dmesh_backends();
    let metrics = app.dmesh_metrics();
    let drain = app.spawn();
    let _ = events_tx.send(DmeshEvent::InfraReady);

    Ok(Some(Worker {
        id: worker_id,
        _drain: Box::new(drain),
        events: events_tx,
        registrations,
        sessions: HashMap::new(),
        by_conn: HashMap::new(),
        order: Vec::new(),
        drain_next: 0,
        slots: Slots::new(worker_id.max(0) as u16),
        pending: HashMap::new(),
        backends,
        metrics,
        tx_reserve: tx_reserve_enabled(),
        counters: Counters::default(),
    }))
}

/// Output path selection. The reservation path copies once, into the egress
/// arena; `DMESH_L7_TX_RESERVE=0` selects the copy-then-send path it replaced,
/// which is what makes the two comparable on hardware.
#[cfg(not(test))]
fn tx_reserve_enabled() -> bool {
    std::env::var("DMESH_L7_TX_RESERVE").map_or(true, |v| v != "0")
}

// ---- the contract ----

/// # Safety
/// `driver` must be the DPUmesh context for `worker_id` and remain valid until
/// the worker is stopped.
#[cfg(not(test))]
#[no_mangle]
pub unsafe extern "C" fn l7_worker_run(worker_id: c_int, driver: *mut c_void) -> c_int {
    if driver.is_null() {
        return -1;
    }
    let runtime = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => {
            eprintln!("[l7_linkerd] worker {worker_id} runtime failed: {error}");
            dmesh_l7_driver_failed(driver);
            return -1;
        }
    };

    let result = runtime.block_on(async {
        match build_worker(worker_id).await {
            Ok(Some(worker)) => {
                let summary = worker.counters.summary();
                WORKER.with(|slot| *slot.borrow_mut() = Some(worker));
                eprintln!("[l7_linkerd] worker {worker_id} running: {summary}");
            }
            Ok(None) => {
                eprintln!("[l7_linkerd] worker {worker_id} running without Linkerd sessions");
            }
            Err(error) => return Err(io::Error::other(error)),
        }

        dmesh_doca::runtime::run(ExternalBackend { worker_id, driver }).await
    });

    detach_worker(worker_id);
    if let Err(error) = result {
        eprintln!("[l7_linkerd] worker {worker_id} driver failed: {error}");
        dmesh_l7_driver_failed(driver);
        -1
    } else {
        0
    }
}

/// Drive one worker's endpoints from a test; the runtime backend calls
/// `collect_registrations` and `drain` directly.
#[cfg(test)]
fn drain_worker(worker_id: c_int) -> c_int {
    with_worker(worker_id, 0, |w| {
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
        return DECLINE_MODE;
    }
    with_worker(worker_id, DECLINE_NOT_ATTACHED, |w| {
        // Connection handles encode the pod in one byte.
        let key_pod = if flow.is_reply != 0 {
            flow.peer_pod
        } else {
            flow.src_pod
        };
        if !(0..=0xff).contains(&key_pod) {
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
        return -1;
    }
    with_worker(worker_id, -1, |w| {
        let Some(&key) = w.by_conn.get(&conn) else {
            return -1;
        };
        let Some(s) = w.sessions.get_mut(&key) else {
            return -1;
        };
        // Route input to its session direction.
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
/// Called on the owning worker thread.
#[no_mangle]
pub unsafe extern "C" fn l7_conn_close(worker_id: c_int, conn: u64) {
    with_worker(worker_id, (), |w| {
        // Either transport direction ending invalidates the paired stream.
        if let Some(key) = w.by_conn.get(&conn).copied() {
            w.close_session(key);
        }
    });
}

fn detach_worker(worker_id: c_int) {
    // Close sessions and release their staging custody.
    let mine = with_worker(worker_id, false, |w| {
        for key in w.order.clone() {
            w.close_session(key);
        }
        true
    });
    if !mine {
        return;
    }
    WORKER.with(|slot| {
        if let Some(w) = slot.borrow_mut().take() {
            let (acquired, contended) = dmesh_doca::lock_stats();
            let (registry_acquired, registry_contended) = w.backends.lock_stats();
            eprintln!(
                "[l7_linkerd] worker {worker_id} detached: {} sessions left; {}; {}; \
                 endpoint_lock={acquired}/{contended} \
                 registry_lock={registry_acquired}/{registry_contended}",
                w.sessions.len(),
                w.counters.summary(),
                w.metrics.summary()
            );
        }
    });
}

/// Decline decision-mode handling.
///
/// # Safety
/// `flow` and `out` must be valid for the call.
#[no_mangle]
pub unsafe extern "C" fn l7_resolve(
    _worker_id: c_int,
    _flow: *const DmeshL7Flow,
    _out: *mut DmeshL7Verdict,
) -> c_int {
    -1
}

/// Decision-mode terminal report entry point.
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

/// ABI checks for `dmesh_l7.h`.
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
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    /// Adapter worker with test endpoints and datapath calls.
    struct TestWorker {
        events: mpsc::UnboundedReceiver<DmeshEvent>,
        registrar: mpsc::UnboundedSender<Registration>,
        backends: Arc<Backends>,
        metrics: Arc<SessionMetrics>,
    }

    fn install_worker(id: c_int) -> TestWorker {
        let (events_tx, events) = mpsc::unbounded_channel();
        let (registrar, registrations) = mpsc::unbounded_channel();
        let backends = Arc::new(Backends::new());
        let metrics = Arc::new(SessionMetrics::default());
        let w = Worker {
            id,
            _drain: Box::new(()),
            events: events_tx,
            registrations,
            sessions: HashMap::new(),
            by_conn: HashMap::new(),
            order: Vec::new(),
            drain_next: 0,
            slots: Slots::new(id.max(0) as u16),
            pending: HashMap::new(),
            backends: backends.clone(),
            metrics: metrics.clone(),
            tx_reserve: true,
            counters: Counters::default(),
        };
        WORKER.with(|slot| *slot.borrow_mut() = Some(w));
        fake::reset();
        TestWorker {
            events,
            registrar,
            backends,
            metrics,
        }
    }

    fn with_test_worker<R>(f: impl FnOnce(&mut Worker) -> R) -> R {
        WORKER.with(|slot| f(slot.borrow_mut().as_mut().unwrap()))
    }

    /// The token the adapter gave the session opened on `key`.
    fn token_of(key: u64) -> SessionToken {
        with_test_worker(|w| w.sessions[&key].token)
    }

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

    /// Register a client endpoint for a session, as the acceptor would.
    fn register_client(tw: &TestWorker, token: SessionToken) -> DmeshIo {
        let (io, handle) = dmesh_doca::dmesh_io_pair("10.97.0.1:1".parse().unwrap());
        tw.registrar.send(Registration { token, handle }).unwrap();
        with_test_worker(|w| w.collect_registrations());
        io
    }

    fn take_backend(tw: &TestWorker, service: i32, conn: u64) -> DmeshIo {
        let key = BackendKey::new(service_addr(service), token_of(conn));
        tw.backends
            .take(&key)
            .expect("the session published its backend channel")
    }

    fn write_to(io: &mut DmeshIo, bytes: &[u8]) {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        rt.block_on(async { io.write_all(bytes).await.unwrap() });
    }

    fn read_eof(io: &mut DmeshIo) {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        rt.block_on(async {
            let mut byte = [0u8; 1];
            assert_eq!(io.read(&mut byte).await.unwrap(), 0, "the endpoint is EOF");
            assert_eq!(
                io.write_all(b"late").await.unwrap_err().kind(),
                std::io::ErrorKind::BrokenPipe
            );
        });
    }

    fn sent() -> Vec<(u64, i32, Vec<u8>)> {
        fake::STATE.with(|s| std::mem::take(&mut s.borrow_mut().sent))
    }

    fn released() -> Vec<(u64, u32, u32)> {
        fake::STATE.with(|s| s.borrow_mut().released.clone())
    }

    fn cancels() -> usize {
        fake::STATE.with(|s| s.borrow().cancels)
    }

    #[test]
    fn session_key_matches_c_handle() {
        // Shared C and Rust handle vectors.
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
    fn same_service_sessions_have_distinct_backend_keys_and_close_independently() {
        let tw = install_worker(0);
        let first = request_flow(21, 1, 4001);
        let second = request_flow(21, 2, 4002);
        let first_key = session_key(1, 4001);
        let second_key = session_key(2, 4002);
        assert_eq!(unsafe { l7_conn_open(0, first_key, &first) }, 0);
        assert_eq!(unsafe { l7_conn_open(0, second_key, &second) }, 0);
        let first_token = token_of(first_key);
        let second_token = token_of(second_key);
        assert_ne!(first_token, second_token);
        with_test_worker(|w| {
            assert_eq!(w.sessions.len(), 2);
            assert_eq!(w.order.len(), 2);
            assert_eq!(w.by_conn.get(&first_key), Some(&first_key));
            assert_eq!(w.by_conn.get(&second_key), Some(&second_key));
            assert_eq!(w.counters.connections_declined, 0);
        });
        assert_eq!(
            tw.backends.sessions_for(&service_addr(21)),
            vec![first_token, second_token]
        );

        let mut first_backend = take_backend(&tw, 21, first_key);
        let mut second_backend = take_backend(&tw, 21, second_key);
        let mut first_client = register_client(&tw, first_token);
        let mut second_client = register_client(&tw, second_token);
        assert_eq!(unsafe { l7_conn_open(0, 101, &reply_flow(21, 1, 4001)) }, 0);
        assert_eq!(unsafe { l7_conn_open(0, 102, &reply_flow(21, 2, 4002)) }, 0);

        unsafe { l7_conn_close(0, 101) };
        read_eof(&mut first_client);
        read_eof(&mut first_backend);
        assert_eq!(
            tw.backends.sessions_for(&service_addr(21)),
            vec![second_token],
            "closing the first same-service session keeps the second live"
        );
        with_test_worker(|w| {
            assert_eq!(w.sessions.len(), 1);
            assert!(w.sessions.contains_key(&second_key));
        });

        // Reusing the first slot creates a new generation without disturbing
        // the other same-service session.
        let third = request_flow(21, 3, 4003);
        let third_key = session_key(3, 4003);
        assert_eq!(unsafe { l7_conn_open(0, third_key, &third) }, 0);
        let third_token = token_of(third_key);
        assert_eq!(third_token.slot, first_token.slot);
        assert_ne!(third_token.generation, first_token.generation);
        let mut third_backend = take_backend(&tw, 21, third_key);
        let mut third_client = register_client(&tw, third_token);
        assert_eq!(
            tw.backends.sessions_for(&service_addr(21)),
            vec![second_token, third_token]
        );

        // Both surviving sessions still own writable endpoints.
        write_to(&mut second_client, b"client-two");
        write_to(&mut second_backend, b"backend-two");
        write_to(&mut third_client, b"client-three");
        write_to(&mut third_backend, b"backend-three");

        unsafe { l7_conn_close(0, third_key) };
        read_eof(&mut third_client);
        read_eof(&mut third_backend);
        assert_eq!(
            tw.backends.sessions_for(&service_addr(21)),
            vec![second_token]
        );
        unsafe { l7_conn_close(0, second_key) };
        read_eof(&mut second_client);
        read_eof(&mut second_backend);
        assert!(!tw.backends.contains_service(&service_addr(21)));
    }

    /// Sessions to different services run side by side, each with its own
    /// token and its own backend key.
    #[test]
    fn concurrent_sessions_hold_distinct_tokens_and_keys() {
        let tw = install_worker(0);
        let first = request_flow(40, 1, 4101);
        let second = request_flow(41, 2, 4102);
        assert_eq!(unsafe { l7_conn_open(0, 1, &first) }, 0);
        assert_eq!(unsafe { l7_conn_open(0, 2, &second) }, 0);

        let (a, b) = (token_of(1), token_of(2));
        assert_ne!(a, b);
        assert_eq!(tw.backends.sessions_for(&service_addr(40)), vec![a]);
        assert_eq!(tw.backends.sessions_for(&service_addr(41)), vec![b]);
        assert_eq!(tw.metrics.sessions_active.get(), 2);

        with_test_worker(|w| w.close_session(1));
        assert!(!tw.backends.contains_service(&service_addr(40)));
        assert_eq!(
            tw.backends.sessions_for(&service_addr(41)),
            vec![b],
            "closing one session leaves the other's channel alone"
        );
        with_test_worker(|w| w.close_session(2));
        assert_eq!(tw.metrics.sessions_active.get(), 0);
    }

    /// A slot handed out again is a new session: the closed generation's
    /// registration is refused rather than bound to its successor.
    #[test]
    fn a_stale_registration_never_binds_to_the_next_generation() {
        let tw = install_worker(0);
        let flow = request_flow(42, 3, 4201);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let old = token_of(1);
        with_test_worker(|w| w.close_session(1));

        let next = request_flow(42, 4, 4202);
        assert_eq!(unsafe { l7_conn_open(0, 2, &next) }, 0);
        let new = token_of(2);
        assert_eq!(new.slot, old.slot, "the slot is reused");
        assert_ne!(new.generation, old.generation);

        let mut stale = register_client(&tw, old);
        with_test_worker(|w| {
            assert!(
                w.sessions[&2].client.handle.is_none(),
                "generation {} must not receive generation {}'s endpoint",
                new.generation,
                old.generation
            );
            assert_eq!(w.counters.registrations_orphaned, 1);
        });
        read_eof(&mut stale);
        assert_eq!(tw.metrics.registrations_orphaned.get(), 1);

        let _live = register_client(&tw, new);
        with_test_worker(|w| {
            assert!(w.sessions[&2].client.handle.is_some());
            assert_eq!(w.pending.len(), 0);
            w.close_session(2);
        });
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
        let tw = install_worker(0);
        let rep = reply_flow(23, 9, 4009);
        assert_eq!(
            unsafe { l7_conn_open(0, 4242, &rep) },
            DECLINE_UNKNOWN_REPLY
        );
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert!(w.by_conn.is_empty());
        });
        assert!(!tw.backends.contains_service(&service_addr(23)));
    }

    /// Open a session whose backend endpoint has `bytes` waiting to go out.
    fn session_with_backend_output(
        tw: &TestWorker,
        service: i32,
        conn: u64,
        bytes: &[u8],
    ) -> DmeshIo {
        let flow = request_flow(service, 5, 5000);
        assert_eq!(unsafe { l7_conn_open(0, conn, &flow) }, 0);
        let mut io = take_backend(tw, service, conn);
        write_to(&mut io, bytes);
        io
    }

    #[test]
    fn full_send_does_not_requeue() {
        let tw = install_worker(0);
        let _io = session_with_backend_output(&tw, 24, 1, b"0123456789");
        assert_eq!(drain_worker(0), 1);
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123456789".to_vec())]);
        // Nothing was put back, so a second step has nothing to publish.
        drain_worker(0);
        assert!(sent().is_empty());
        with_test_worker(|w| {
            assert_eq!(w.counters.send_retries, 0);
            assert_eq!(w.counters.bytes_to_backend, 10);
            w.close_session(1);
        });
    }

    /// A refused publication cancels the reservation and leaves every byte
    /// queued, in order, for the next pass.
    #[test]
    fn a_refused_reservation_requeues_every_byte() {
        let tw = install_worker(0);
        let _io = session_with_backend_output(&tw, 25, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().accept = Some(0));
        drain_worker(0);
        assert!(sent().is_empty());
        assert_eq!(cancels(), 0, "the datapath refused, it was not cancelled");

        fake::STATE.with(|s| s.borrow_mut().accept = None);
        drain_worker(0);
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123456789".to_vec())]);
        with_test_worker(|w| {
            assert_eq!(w.counters.send_retries, 1);
            w.close_session(1);
        });
    }

    /// Output larger than one chunk is published in order, within one pass.
    #[test]
    fn a_pass_fills_several_reservations_in_order() {
        let tw = install_worker(0);
        fake::STATE.with(|s| s.borrow_mut().chunk = 4);
        let _io = session_with_backend_output(&tw, 45, 1, b"0123456789");
        drain_worker(0);
        assert_eq!(
            sent(),
            vec![
                (1, BACKEND_ANY, b"0123".to_vec()),
                (1, BACKEND_ANY, b"4567".to_vec()),
                (1, BACKEND_ANY, b"89".to_vec()),
            ]
        );
        with_test_worker(|w| {
            assert_eq!(w.counters.bytes_to_backend, 10);
            w.close_session(1);
        });
    }

    /// A pass with nothing to write cancels its reservation with length 0
    /// instead of leaving the chunk lent out.
    #[test]
    fn an_empty_pass_holds_no_reservation() {
        let tw = install_worker(0);
        let flow = request_flow(43, 6, 4301);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let _backend = take_backend(&tw, 43, 1);
        drain_worker(0);
        assert!(sent().is_empty());
        assert_eq!(cancels(), 0, "an empty queue reserves nothing at all");
        with_test_worker(|w| w.close_session(1));
    }

    /// A chunk the arena cannot lend is a stall, not a loss.
    #[test]
    fn an_exhausted_arena_keeps_the_bytes() {
        let tw = install_worker(0);
        let _io = session_with_backend_output(&tw, 44, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().no_chunk = true);
        drain_worker(0);
        assert!(sent().is_empty());
        with_test_worker(|w| {
            assert_eq!(w.counters.send_errors, 0);
            assert_eq!(w.sessions.len(), 1, "a dry arena does not close a session");
        });

        fake::STATE.with(|s| s.borrow_mut().no_chunk = false);
        drain_worker(0);
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123456789".to_vec())]);
        with_test_worker(|w| w.close_session(1));
    }

    /// The compatibility path publishes the same bytes, and a partial accept
    /// restores only the unaccepted suffix.
    #[test]
    fn the_copy_path_requeues_only_the_suffix() {
        let tw = install_worker(0);
        with_test_worker(|w| w.tx_reserve = false);
        let _io = session_with_backend_output(&tw, 26, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().accept = Some(4));
        drain_worker(0);
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"0123".to_vec())]);
        fake::STATE.with(|s| s.borrow_mut().accept = None);
        drain_worker(0);
        // The suffix, once, in order: no byte is offered twice.
        assert_eq!(sent(), vec![(1, BACKEND_ANY, b"456789".to_vec())]);
        with_test_worker(|w| w.close_session(1));
    }

    #[test]
    fn over_accept_is_terminal() {
        let tw = install_worker(0);
        let _io = session_with_backend_output(&tw, 27, 1, b"0123456789");
        fake::STATE.with(|s| s.borrow_mut().over_accept = true);
        drain_worker(0);
        with_test_worker(|w| {
            assert_eq!(w.counters.send_errors, 1);
            assert!(w.sessions.is_empty(), "the session is closed, not resumed");
        });
        assert!(!tw.backends.contains_service(&service_addr(27)));
    }

    #[test]
    fn close_releases_every_outstanding_extent_once() {
        let tw = install_worker(0);
        let flow = request_flow(28, 6, 6000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let _client = register_client(&tw, token_of(1));
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
        assert!(!tw.backends.contains_service(&service_addr(28)));
    }

    #[test]
    fn reply_close_aborts_session_and_allows_same_service_to_reopen() {
        let tw = install_worker(0);
        let flow = request_flow(33, 11, 11000);
        let key = session_key(11, 11000);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        let mut backend = take_backend(&tw, 33, key);
        let mut client = register_client(&tw, token_of(key));

        let reply = reply_flow(33, 11, 11000);
        assert_eq!(unsafe { l7_conn_open(0, 777, &reply) }, 0);
        let staging = [7u8; 32];
        unsafe {
            assert_eq!(l7_conn_segment(0, key, staging.as_ptr(), 0, 8), 8);
            assert_eq!(l7_conn_segment(0, 777, staging.as_ptr(), 8, 8), 8);
        }
        write_to(&mut client, b"unsent-origin");
        write_to(&mut backend, b"unsent-backend");

        unsafe { l7_conn_close(0, 777) };

        assert_eq!(released(), vec![(key, 0, 8), (777, 8, 8)]);
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert!(w.by_conn.is_empty());
            assert!(w.order.is_empty());
        });
        read_eof(&mut client);
        read_eof(&mut backend);

        let next = request_flow(33, 12, 12000);
        let next_key = session_key(12, 12000);
        assert_eq!(unsafe { l7_conn_open(0, next_key, &next) }, 0);
        assert!(tw.backends.contains_service(&service_addr(33)));
        unsafe { l7_conn_close(0, next_key) };
    }

    #[test]
    fn registration_after_close_is_aborted() {
        let tw = install_worker(0);
        let flow = request_flow(34, 13, 13000);
        let key = session_key(13, 13000);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        let token = token_of(key);
        unsafe { l7_conn_close(0, key) };

        let mut client = register_client(&tw, token);
        read_eof(&mut client);
        assert_eq!(tw.metrics.registrations_orphaned.get(), 1);
    }

    #[test]
    fn stack_endpoint_drop_closes_session_and_allows_reopen() {
        let tw = install_worker(0);
        let flow = request_flow(35, 14, 14000);
        let key = session_key(14, 14000);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        let client = register_client(&tw, token_of(key));
        drop(client);

        assert_eq!(drain_worker(0), 1);
        with_test_worker(|w| {
            assert!(w.sessions.is_empty());
            assert!(w.by_conn.is_empty());
            assert!(w.order.is_empty());
        });

        let next = request_flow(35, 15, 15000);
        let next_key = session_key(15, 15000);
        assert_eq!(unsafe { l7_conn_open(0, next_key, &next) }, 0);
        unsafe { l7_conn_close(0, next_key) };
    }

    #[test]
    fn normal_drain_then_close_does_not_double_release() {
        let tw = install_worker(0);
        let flow = request_flow(29, 7, 7000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let mut client = register_client(&tw, token_of(1));
        let staging = vec![7u8; 4096];
        unsafe { assert_eq!(l7_conn_segment(0, 1, staging.as_ptr(), 0, 16), 16) };

        // The next drain pass returns the consumed segment.
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        rt.block_on(async {
            let mut buf = [0u8; 16];
            client.read_exact(&mut buf).await.unwrap();
        });
        drain_worker(0);
        assert_eq!(released(), vec![(1, 0, 16)]);

        unsafe { l7_conn_close(0, 1) };
        assert_eq!(released(), vec![(1, 0, 16)], "released once, not twice");
        with_test_worker(|w| assert_eq!(w.counters.segments_released, 1));
    }

    /// Every way a session can end, run to quiescence. Each sequence must
    /// leave no session state behind, return every extent exactly once, and
    /// leave the service free to open again.
    #[test]
    fn closing_is_idempotent_however_it_arrives() {
        struct Case {
            name: &'static str,
            service: i32,
            /// What ends the session, in order, after both extents are handed over.
            run: fn(u64, u64),
        }

        // Handles: `req` is the request connection, `rep` the reply direction.
        fn eof_request_then_two_closes(req: u64, _rep: u64) {
            unsafe {
                l7_conn_eof(0, req);
                l7_conn_close(0, req);
                l7_conn_close(0, req);
            }
        }
        fn eof_reply_then_close_both(req: u64, rep: u64) {
            unsafe {
                l7_conn_eof(0, rep);
                l7_conn_close(0, rep);
                l7_conn_close(0, req);
            }
        }
        fn close_request_then_reply(req: u64, rep: u64) {
            unsafe {
                l7_conn_close(0, req);
                l7_conn_close(0, rep);
            }
        }
        fn close_request_then_detach(req: u64, _rep: u64) {
            unsafe { l7_conn_close(0, req) };
            detach_worker(0);
        }
        fn close_reply_then_detach(_req: u64, rep: u64) {
            unsafe { l7_conn_close(0, rep) };
            detach_worker(0);
        }
        fn terminal_send_then_close(req: u64, _rep: u64) {
            fake::STATE.with(|s| s.borrow_mut().fail = true);
            drain_worker(0);
            fake::STATE.with(|s| s.borrow_mut().fail = false);
            unsafe { l7_conn_close(0, req) };
        }

        let cases = [
            Case {
                name: "eof(request), close(request), close(request)",
                service: 50,
                run: eof_request_then_two_closes,
            },
            Case {
                name: "eof(reply), close(reply), close(request)",
                service: 51,
                run: eof_reply_then_close_both,
            },
            Case {
                name: "close(request), close(reply)",
                service: 52,
                run: close_request_then_reply,
            },
            Case {
                name: "close(request), detach_worker",
                service: 53,
                run: close_request_then_detach,
            },
            Case {
                name: "close(reply), detach_worker",
                service: 54,
                run: close_reply_then_detach,
            },
            Case {
                name: "terminal send failure, close(request)",
                service: 55,
                run: terminal_send_then_close,
            },
        ];

        for case in cases {
            let tw = install_worker(0);
            let pod = 20;
            let port = 5500;
            let req = session_key(pod, port);
            let rep = 900;
            let flow = request_flow(case.service, pod, port);
            assert_eq!(unsafe { l7_conn_open(0, req, &flow) }, 0, "{}", case.name);
            let token = token_of(req);
            let mut backend = take_backend(&tw, case.service, req);
            let mut client = register_client(&tw, token);
            assert_eq!(
                unsafe { l7_conn_open(0, rep, &reply_flow(case.service, pod, port)) },
                0,
                "{}",
                case.name
            );

            // One extent per direction, and output waiting in both endpoints.
            let staging = [7u8; 64];
            unsafe {
                assert_eq!(l7_conn_segment(0, req, staging.as_ptr(), 0, 8), 8);
                assert_eq!(l7_conn_segment(0, rep, staging.as_ptr(), 8, 16), 16);
            }
            write_to(&mut client, b"to-origin");
            write_to(&mut backend, b"to-backend");

            (case.run)(req, rep);

            assert_eq!(
                released(),
                vec![(req, 0, 8), (rep, 8, 16)],
                "{}: every extent returns exactly once",
                case.name
            );
            read_eof(&mut client);
            read_eof(&mut backend);
            assert!(
                !tw.backends.contains_service(&service_addr(case.service)),
                "{}: the backend registry entry is gone",
                case.name
            );

            let quiesced = WORKER.with(|slot| slot.borrow().is_some());
            if quiesced {
                with_test_worker(|w| {
                    assert!(w.sessions.is_empty(), "{}: sessions", case.name);
                    assert!(w.by_conn.is_empty(), "{}: by_conn", case.name);
                    assert!(w.pending.is_empty(), "{}: pending", case.name);
                    assert!(w.order.is_empty(), "{}: order", case.name);
                    assert_eq!(w.counters.segments_released, 2, "{}", case.name);
                });
                // The service opens again, on a new generation of the slot.
                let next = request_flow(case.service, pod + 1, port + 1);
                let next_key = session_key(pod + 1, port + 1);
                assert_eq!(
                    unsafe { l7_conn_open(0, next_key, &next) },
                    0,
                    "{}",
                    case.name
                );
                let next_token = token_of(next_key);
                assert_ne!(
                    next_token, token,
                    "{}: a reused slot is a new session",
                    case.name
                );
                unsafe { l7_conn_close(0, next_key) };
            }
            assert_eq!(
                tw.metrics.sessions_active.get(),
                0,
                "{}: no session is left active",
                case.name
            );
            assert!(
                tw.metrics.quiescent(),
                "{}: {}",
                case.name,
                tw.metrics.summary()
            );
            detach_worker(0);
        }
    }

    #[test]
    fn wrong_worker_id_is_rejected() {
        let tw = install_worker(0);
        let flow = request_flow(30, 8, 8000);
        assert_eq!(
            unsafe { l7_conn_open(1, 1, &flow) },
            DECLINE_NOT_ATTACHED,
            "another worker's runtime is not this thread's to open on"
        );
        assert_eq!(drain_worker(1), 0);
        assert_eq!(
            unsafe { l7_conn_segment(1, 1, [0u8; 8].as_ptr(), 0, 8) },
            -1
        );
        unsafe { l7_conn_close(1, 1) };
        detach_worker(1);
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
        assert!(!tw.backends.contains_service(&service_addr(30)));
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
            !tw.backends.contains_service(&service_addr(31)),
            "a published channel must not outlive the session that published it"
        );
    }

    #[test]
    fn detach_releases_outstanding_custody() {
        let tw = install_worker(0);
        let flow = request_flow(32, 10, 10000);
        assert_eq!(unsafe { l7_conn_open(0, 1, &flow) }, 0);
        let _client = register_client(&tw, token_of(1));
        let staging = vec![7u8; 4096];
        unsafe { assert_eq!(l7_conn_segment(0, 1, staging.as_ptr(), 64, 8), 8) };
        detach_worker(0);
        assert_eq!(released(), vec![(1, 64, 8)]);
        assert!(!tw.backends.contains_service(&service_addr(32)));
        WORKER.with(|slot| assert!(slot.borrow().is_none()));
    }
}
