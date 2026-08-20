//! Embedded Linkerd outbound adapter for DPUmesh.
//!
//! DPUmesh owns DOCA, progress engines, DMA rings and worker threads. Each ARM
//! worker hosts a Tokio `current_thread` runtime and the persistent driver in
//! `dmesh_doca::runtime`. One configured worker, or every worker under the
//! `all` selection, carries Linkerd sessions.

use std::cell::Cell;
use std::collections::HashMap;
#[cfg(not(test))]
use std::ffi::c_void;
#[cfg(not(test))]
use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::os::raw::{c_char, c_int};
use std::path::{Path, PathBuf};
use std::sync::Arc;
#[cfg(not(test))]
use std::task::{Context, Poll};
use std::time::{Duration, SystemTime};

/// Bytes one feed generation may hold. A larger document is refused unread.
const MAX_FEED_BYTES: u64 = 256 * 1024;

/// Read one feed generation, refusing a document larger than a generation may be.
fn read_feed(path: &Path) -> Result<String, String> {
    let len = std::fs::metadata(path)
        .map_err(|error| format!("stat {}: {error}", path.display()))?
        .len();
    if len > MAX_FEED_BYTES {
        return Err(format!(
            "service target feed is {len} bytes, over the {MAX_FEED_BYTES}-byte bound"
        ));
    }
    std::fs::read_to_string(path).map_err(|error| format!("read {}: {error}", path.display()))
}

/// How long a feed generation must have been installed before its stamp is
/// trusted to mean "unchanged".
const STAMP_SETTLE: Duration = Duration::from_secs(2);

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
    pub workload: [c_char; 384],
    pub source_identity: [c_char; 254],
}

/// Read one NUL-terminated fixed-width field of a flow.
fn flow_text(bytes: &[c_char]) -> String {
    let end = bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len());
    #[allow(clippy::unnecessary_cast)]
    let raw: Vec<u8> = bytes[..end].iter().map(|&c| c as u8).collect();
    String::from_utf8_lossy(&raw).into_owned()
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

/// Reservations one connection may publish in a drain pass. A reservation is one
/// egress chunk, so this matches the copy path's `dmesh_l7_send` cap.
const TX_RESERVATIONS_MAX: usize = 4;

/// Aggregate output and session budgets for one drain pass.
const DRAIN_MAX: usize = 256 * 1024;
const DRAIN_SESSIONS_MAX: usize = 64;

/// Port used in synthetic service addresses.
const SERVICE_PORT: u16 = 9092;

/// Resolve a `namespace/name` Service key to this node's DPU-interned id,
/// answered from the held topology generation. `None` until a generation
/// interns it; the publisher republishes, so a later generation retries.
#[cfg(not(test))]
fn resolve_service_key(worker_id: c_int, key: &str) -> Option<i32> {
    extern "C" {
        fn dmesh_l7_svc_for_name(worker_id: c_int, key: *const c_char) -> i32;
    }
    let key = std::ffi::CString::new(key).ok()?;
    let id = unsafe { dmesh_l7_svc_for_name(worker_id, key.as_ptr()) };
    (0..=i8::MAX as i32).contains(&id).then_some(id)
}

/// `DMESH_L7_ENDPOINT_*` ABI values.
const ENDPOINT_REMOTE: i32 = -2;
const ENDPOINT_STALE: i32 = -3;

/// Resolve the Pod UID behind an endpoint the balancer selected to the live
/// destination it names, or to the reason it names none.
#[cfg(not(test))]
fn resolve_endpoint_uid(worker_id: c_int, uid: &str) -> i32 {
    extern "C" {
        fn dmesh_l7_pod_for_uid(worker_id: c_int, pod_uid: *const c_char) -> i32;
    }
    let Ok(uid) = std::ffi::CString::new(uid) else {
        return -1;
    };
    unsafe { dmesh_l7_pod_for_uid(worker_id, uid.as_ptr()) }
}

/// Test resolution: one live Pod, one placed elsewhere, one whose mapping the
/// held generation no longer names.
#[cfg(test)]
fn resolve_endpoint_uid(_worker_id: c_int, uid: &str) -> i32 {
    match uid {
        "11111111-1111-1111-1111-111111111111" => 7,
        "22222222-2222-2222-2222-222222222222" => ENDPOINT_REMOTE,
        "33333333-3333-3333-3333-333333333333" => ENDPOINT_STALE,
        _ => -1,
    }
}

/// Test interning: the generation names two Services.
#[cfg(test)]
fn resolve_service_key(_worker_id: c_int, key: &str) -> Option<i32> {
    match key {
        "test-bench/echo-a" => Some(11),
        "test-bench/echo-b" => Some(20),
        _ => None,
    }
}

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

// Length of the signed prefix of an authoritative feed document, or -1 when it
// is unsigned or its signature does not verify against the registration keyring
// the DPU already holds. The crypto stays on the C side, so the adapter keeps no
// key material of its own.
#[cfg(not(test))]
extern "C" {
    fn dmesh_l7_verify_feed(document: *const u8, length: usize) -> isize;
}

/// Test stand-in: the envelope handling and the failure paths are what the
/// adapter owns; the MAC itself is covered by `tests/workload_grant_test.c`.
#[cfg(test)]
unsafe fn dmesh_l7_verify_feed(document: *const u8, length: usize) -> isize {
    let text = std::str::from_utf8(std::slice::from_raw_parts(document, length)).unwrap();
    match text.rfind("\nsignature=") {
        Some(at) if text[at + 1..].trim_end() == "signature=test,valid" => at as isize + 1,
        _ => -1,
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

/// Log the first event and every 4096th event.
fn rate_limited(count: u64) -> bool {
    count == 1 || count.is_multiple_of(4096)
}

/// Adapter decline categories.
#[derive(Clone, Copy)]
enum Decline {
    Error,
    /// The Service target feed could not be read, parsed, or had rolled back.
    FeedRejected,
    /// The Service the session names is absent from the current generation.
    TargetWithdrawn,
    /// No session token was available.
    NoSlot,
    /// The backend registry refused this session's channel.
    BackendRefused,
    SessionLimit,
    UnknownReply,
}

impl Decline {
    /// The DPUmesh-visible code. The finer causes stay Rust-side labels so the
    /// datapath ABI does not grow a code per diagnosis.
    fn code(self) -> c_int {
        match self {
            Decline::Error
            | Decline::FeedRejected
            | Decline::TargetWithdrawn
            | Decline::NoSlot
            | Decline::BackendRefused => DECLINE_ERROR,
            Decline::SessionLimit => DECLINE_SESSION_LIMIT,
            Decline::UnknownReply => DECLINE_UNKNOWN_REPLY,
        }
    }

    fn reason(self) -> &'static str {
        match self {
            Decline::Error => "adapter-error",
            Decline::FeedRejected => "feed-rejected",
            Decline::TargetWithdrawn => "target-withdrawn",
            Decline::NoSlot => "no-session-slot",
            Decline::BackendRefused => "backend-refused",
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

/// Service targets and their ready endpoints as the controller feed names them:
/// by `namespace/name` Service key, each endpoint carrying the Pod UID the feed
/// names it by.
type NamedServiceTargets = HashMap<String, SocketAddrV4>;
type NamedServiceEndpoints = HashMap<String, Vec<(SocketAddr, String)>>;

/// The same two maps once resolved, keyed by the compact Service id the DPU
/// interns each key to.
type ServiceTargets = HashMap<i32, SocketAddrV4>;
type ServiceEndpoints = HashMap<i32, Vec<(SocketAddr, String)>>;

/// One parsed feed generation: its version and the maps it carries.
type ServiceFeed = (u64, NamedServiceTargets, NamedServiceEndpoints);

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
    /// Real Kubernetes destination presented to Linkerd for each DPUmesh
    /// service. The synthetic address remains the internal backend key.
    service_targets: ServiceTargets,
    /// Ready endpoints in the same authoritative Service generation, each with
    /// the Pod UID the generation names it by. The address is what the Linkerd
    /// balancer selects; the UID is what resolves it to a live destination,
    /// and a recreated Pod carries a new one, so a mapping cannot be
    /// inherited.
    service_endpoints: ServiceEndpoints,
    /// Atomically replaced, monotonically versioned controller feed.
    service_targets_file: Option<PathBuf>,
    service_targets_version: u64,
    service_targets_authoritative: bool,
    /// Inode, modification time and length of the feed generation already
    /// parsed. The publisher installs a generation by rename, so each one
    /// arrives on its own inode; modification time alone would not separate two
    /// generations installed within a filesystem timestamp tick.
    service_targets_stamp: Option<(u64, SystemTime, u64)>,
    /// Copy output into the egress arena rather than through a temporary Vec.
    tx_reserve: bool,
    counters: Counters,
}

type InboundPolicyBuilder = Arc<dyn Fn(Arc<str>) -> linkerd_app::DmeshPolicyStore + Send + Sync>;

/// The inbound policy stores this worker holds, one per registered destination
/// Pod, keyed by its workload.
///
/// A sidecar holds one store because it is the proxy for one workload. This
/// proxy is the inbound enforcement point for every Pod its DPU serves, so the
/// cost scales with destination Pods and ports — one watch per Pod and port,
/// shared by every stream that arrives at it — rather than with sessions. A
/// stream pays an evaluation, not a session build.
///
/// It is deliberately not a field of `Worker`. The verdict is asked from
/// inside the data path, which is already inside the `Worker` borrow, so
/// reaching it through that borrow would be re-entrant.
#[derive(Default)]
struct InboundPolicies {
    build: Option<InboundPolicyBuilder>,
    cache: HashMap<String, linkerd_app::DmeshPolicyStore>,
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

/// The source address a flow presents.
///
/// It is the Pod's real cluster IP, carried in its signed assertion within a
/// node and in the generation across one. Nothing synthetic may stand in for
/// it: an `AuthorizationPolicy`'s `networks` clause is matched before its
/// identity clause and an empty match denies, so a made-up address would make
/// every realistic policy refuse every connection. The port is the source
/// port; zero would not be a socket address, so it reads as one.
fn source_addr(flow: &DmeshL7Flow) -> SocketAddrV4 {
    SocketAddrV4::new(
        Ipv4Addr::from(flow.src_ip),
        if flow.src_port == 0 { 1 } else { flow.src_port },
    )
}

thread_local! {
    static WORKER: std::cell::RefCell<Option<Worker>> =
        const { std::cell::RefCell::new(None) };
    /// Count calls naming another worker.
    static FOREIGN_CALLS: Cell<u64> = const { Cell::new(0) };
    /// The inbound policy stores, borrowed independently of the worker.
    static INBOUND: std::cell::RefCell<InboundPolicies> =
        std::cell::RefCell::new(InboundPolicies::default());
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
/// This explicit compatibility/comparison path is selected at worker startup;
/// it is not an automatic fallback when the reservation path has no chunk.
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

/// What one pass over an endpoint did, and what the caller must decide on next.
#[derive(Clone, Copy)]
struct Pumped {
    /// The pass published output or returned staging custody.
    progressed: bool,
    /// The stack finished this endpoint's write half, so the session is over.
    finished: bool,
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
) -> Result<Pumped, ()> {
    let mut did = false;
    let state = {
        let Some(handle) = side.handle.as_ref() else {
            return Ok(Pumped {
                progressed: false,
                finished: false,
            });
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
        // Both answers are read after publishing, so one lock serves both.
        handle.drain_state()
    };
    // Release a fully consumed input queue.
    if !state.has_rx
        && !side.outstanding.is_empty()
        && side.release_outstanding(worker_id, counters) > 0
    {
        did = true;
    }
    Ok(Pumped {
        progressed: did,
        finished: state.tx_finished,
    })
}

impl Worker {
    /// Publish how the balancer's selected endpoints resolve to live
    /// destinations, alongside the placement snapshot.
    ///
    /// The two answer different questions and both are needed. The placement
    /// says which Service an address belongs to; the resolution says whether
    /// anything is serving it here and now. Every address that is not one of
    /// the session's own must resolve to a live Pod.
    fn place_endpoint_resolution(&self) {
        let mut own: std::collections::HashSet<SocketAddr> = std::collections::HashSet::new();
        let mut by_addr: HashMap<SocketAddr, String> = HashMap::new();
        for (&service, &cluster_ip) in &self.service_targets {
            own.insert(service_addr(service));
            own.insert(SocketAddr::V4(cluster_ip));
        }
        for endpoints in self.service_endpoints.values() {
            for (addr, uid) in endpoints {
                by_addr.insert(*addr, uid.clone());
            }
        }
        let worker = self.id;
        self.backends
            .set_endpoint_resolver(Arc::new(move |selected: SocketAddr| {
                if own.contains(&selected) {
                    return dmesh_doca::EndpointVerdict::SessionOwn;
                }
                // Ports do not participate in identity: the address's IP is
                // what names the Pod, and a Service may expose it on several.
                let Some(uid) = by_addr
                    .get(&selected)
                    .or_else(|| {
                        by_addr
                            .iter()
                            .find(|(addr, _)| addr.ip() == selected.ip())
                            .map(|(_, uid)| uid)
                    })
                    .cloned()
                else {
                    return dmesh_doca::EndpointVerdict::Unresolved;
                };
                match resolve_endpoint_uid(worker, &uid) {
                    pod if pod >= 0 => dmesh_doca::EndpointVerdict::Live,
                    ENDPOINT_REMOTE => dmesh_doca::EndpointVerdict::Remote,
                    ENDPOINT_STALE => dmesh_doca::EndpointVerdict::Stale,
                    _ => dmesh_doca::EndpointVerdict::Unresolved,
                }
            }));
    }

    /// Publish which Service each address the held generation names belongs
    /// to: its session key, its ClusterIP and its ready endpoints. The
    /// connector judges a Linkerd-selected target against this.
    ///
    /// An address two Services both name is left unplaced. `take_session`
    /// refuses a target the generation places in another Service; an unplaced
    /// address stays with the session that selected it.
    fn place_service_targets(&self) {
        let mut placements: HashMap<SocketAddr, SocketAddr> = HashMap::new();
        let mut ambiguous: Vec<SocketAddr> = Vec::new();
        {
            let mut place = |addr: SocketAddr, key: SocketAddr| {
                if let Some(previous) = placements.insert(addr, key) {
                    if previous != key {
                        ambiguous.push(addr);
                    }
                }
            };
            for (&service, &cluster_ip) in &self.service_targets {
                let key = service_addr(service);
                place(key, key);
                place(SocketAddr::V4(cluster_ip), key);
            }
            for (&service, endpoints) in &self.service_endpoints {
                let key = service_addr(service);
                for (endpoint, _uid) in endpoints {
                    place(*endpoint, key);
                }
            }
        }
        for addr in &ambiguous {
            placements.remove(addr);
        }
        self.backends.place_targets(placements);
        self.place_endpoint_resolution();
    }

    /// Adopt the current feed generation. Session open runs this, so it reads
    /// and parses only when the publisher has installed a new generation.
    fn refresh_service_targets(&mut self) -> Result<(), String> {
        let Some(path) = self.service_targets_file.as_deref() else {
            return Ok(());
        };
        let metadata =
            std::fs::metadata(path).map_err(|error| format!("stat {}: {error}", path.display()))?;
        let modified = metadata
            .modified()
            .map_err(|error| format!("mtime {}: {error}", path.display()))?;
        if metadata.len() > MAX_FEED_BYTES {
            return Err(format!(
                "service target feed is {} bytes, over the {MAX_FEED_BYTES}-byte bound",
                metadata.len()
            ));
        }
        let stamp = (
            std::os::unix::fs::MetadataExt::ino(&metadata),
            modified,
            metadata.len(),
        );
        // Skipping the read is an optimization, never a decision: the
        // filesystem reuses inodes across a rename and stamps coarse
        // timestamps, so two generations installed within one tick can share a
        // stamp. A generation is only trusted to be unchanged once it is older
        // than that granularity.
        let settled = SystemTime::now()
            .duration_since(modified)
            .is_ok_and(|age| age >= STAMP_SETTLE);
        if settled && self.service_targets_stamp == Some(stamp) {
            return Ok(());
        }
        let document = read_feed(path)?;
        let (version, targets, endpoints) = parse_signed_service_targets(&document)?;
        if version < self.service_targets_version {
            return Err(format!(
                "service target generation rolled back from {} to {version}",
                self.service_targets_version
            ));
        }
        if version > self.service_targets_version {
            let (resolved_targets, resolved_endpoints) =
                resolve_named_targets(self.id, &targets, &endpoints);
            self.service_targets = resolved_targets;
            self.service_endpoints = resolved_endpoints;
            self.service_targets_version = version;
            self.place_service_targets();
        }
        // Only an accepted generation is stamped, so a rejected rollback is
        // re-read until the publisher installs a newer one.
        self.service_targets_stamp = Some(stamp);
        Ok(())
    }

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
            match (client, backend) {
                (Ok(a), Ok(b)) if !a.finished && !b.finished => did |= a.progressed | b.progressed,
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
        if self.order.last() == Some(&key) {
            self.order.pop();
        } else {
            self.order.retain(|&queued| queued != key);
        }
        if let Some(c) = s.client.conn {
            self.by_conn.remove(&c);
        }
        if let Some(c) = s.backend.conn {
            self.by_conn.remove(&c);
        }
        let token = s.token;
        self.pending.remove(&token);
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
        self.metrics.record_decline(why.reason());
        if rate_limited(self.counters.connections_declined) {
            eprintln!(
                "[l7_linkerd] worker {} declined conn {conn} \
                 (dst_service={} backend_addr={} reason={}) (total {})",
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
            Some((true, _)) => {
                self.by_conn.insert(conn, key);
                self.counters.reply_connections_attached += 1;
                0
            }
        }
    }

    /// Open one request session and publish its backend endpoint.
    fn open_request(&mut self, conn: u64, flow: &DmeshL7Flow) -> c_int {
        let backend_addr = service_addr(flow.dst_service);

        // One live session per connection handle: a second open is refused
        // rather than replacing the entry.
        if self.sessions.contains_key(&conn) {
            return self.decline(Decline::SessionLimit, conn, flow, Some(backend_addr));
        }

        if let Err(error) = self.refresh_service_targets() {
            tracing::warn!(%error, conn, service = flow.dst_service, "service target feed rejected");
            return self.decline(Decline::FeedRejected, conn, flow, Some(backend_addr));
        }

        let workload = flow_text(&flow.workload);
        let src = source_addr(flow);
        let dst = match self.service_targets.get(&flow.dst_service).copied() {
            Some(dst) => dst,
            None if !self.service_targets_authoritative => service_addr_v4(flow.dst_service),
            None => {
                tracing::warn!(conn, service = flow.dst_service, "service target withdrawn");
                return self.decline(Decline::TargetWithdrawn, conn, flow, Some(backend_addr));
            }
        };

        let Some(token) = self.slots.alloc() else {
            eprintln!("[l7_linkerd] worker {}: no session slot left", self.id);
            return self.decline(Decline::NoSlot, conn, flow, Some(backend_addr));
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
            return self.decline(Decline::BackendRefused, conn, flow, Some(backend_addr));
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
        0
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum WorkerSelection {
    One(c_int),
    All,
}

fn parse_worker_selection(value: &str) -> Result<WorkerSelection, String> {
    if value.is_empty() {
        return Ok(WorkerSelection::One(0));
    }
    if value.eq_ignore_ascii_case("all") {
        return Ok(WorkerSelection::All);
    }
    let worker = value.parse::<c_int>().map_err(|_| {
        format!("DPUMESH_L7_LINKERD_WORKER={value} is neither a worker id nor 'all'")
    })?;
    if worker < 0 {
        return Err(format!(
            "DPUMESH_L7_LINKERD_WORKER={value} is neither a nonnegative worker id nor 'all'"
        ));
    }
    Ok(WorkerSelection::One(worker))
}

/// Parses `service-id=IPv4:port` entries separated by commas.
///
/// These addresses are presented to Linkerd destination and policy discovery;
/// DPUmesh's backend channel continues to use its generation-safe session key.
/// A Kubernetes Service is namespace-scoped, so a feed key is always
/// `namespace/name`; a bare name is not a valid identifier anywhere.
fn valid_service_key(key: &str) -> bool {
    match key.split_once('/') {
        Some((namespace, name)) => {
            !namespace.is_empty()
                && !name.is_empty()
                && key.len() <= 127
                && key
                    .chars()
                    .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || "-./".contains(c))
                && !name.contains('/')
        }
        None => false,
    }
}

/// A Kubernetes Pod UID in its canonical RFC 4122 text form. It is the key the
/// endpoint mapping resolves through, and a recreated Pod carries a new one,
/// which is what stops a mapping from being inherited.
fn valid_pod_uid(uid: &str) -> bool {
    uid.len() == 36
        && uid.chars().enumerate().all(|(i, c)| match i {
            8 | 13 | 18 | 23 => c == '-',
            _ => c.is_ascii_hexdigit() && !c.is_ascii_uppercase(),
        })
}

fn parse_service_targets(value: &str) -> Result<NamedServiceTargets, String> {
    let mut targets = HashMap::new();
    for raw in value.split(',').map(str::trim).filter(|s| !s.is_empty()) {
        let (service, addr) = raw.split_once('=').ok_or_else(|| {
            format!("service target entry '{raw}' must be namespace/name=IPv4:port")
        })?;
        let service = service.trim();
        if !valid_service_key(service) {
            return Err(format!(
                "service target entry '{raw}' needs a namespace/name Service key"
            ));
        }
        let addr = addr
            .trim()
            .parse::<SocketAddrV4>()
            .map_err(|_| format!("service target entry '{raw}' needs an IPv4 socket address"))?;
        if targets.insert(service.to_string(), addr).is_some() {
            return Err(format!("service target feed repeats Service {service}"));
        }
    }
    Ok(targets)
}

/// Adopt only the signed prefix of a feed generation.
///
/// The feed carries the same authority as a registration grant, so it is signed
/// by the same keyring. An unsigned or badly signed generation is refused
/// exactly like a malformed one: nothing after the envelope is ever parsed.
fn parse_signed_service_targets(document: &str) -> Result<ServiceFeed, String> {
    let signed = unsafe { dmesh_l7_verify_feed(document.as_ptr(), document.len()) };
    if signed < 0 {
        return Err("service target feed is unsigned or its signature is invalid".to_string());
    }
    let signed = signed as usize;
    if signed > document.len() || !document.is_char_boundary(signed) {
        return Err("service target feed signature covers an invalid prefix".to_string());
    }
    parse_versioned_service_targets(&document[..signed])
}

fn parse_versioned_service_targets(document: &str) -> Result<ServiceFeed, String> {
    let mut version = None;
    let mut entries = Vec::new();
    let mut endpoints: NamedServiceEndpoints = HashMap::new();
    for raw in document.lines() {
        let line = raw.split('#').next().unwrap_or_default().trim();
        if line.is_empty() {
            continue;
        }
        if let Some(value) = line.strip_prefix("version=") {
            if version.is_some() {
                return Err("service target feed repeats version".to_string());
            }
            let parsed = value
                .trim()
                .parse::<u64>()
                .map_err(|_| "service target feed has an invalid version".to_string())?;
            if parsed == 0 {
                return Err("service target feed version must be nonzero".to_string());
            }
            version = Some(parsed);
        } else if let Some(value) = line.strip_prefix("endpoint=") {
            let mut fields = value.split(',');
            let (Some(service), Some(addr), Some(pod_uid), None) =
                (fields.next(), fields.next(), fields.next(), fields.next())
            else {
                return Err(format!(
                    "service endpoint '{line}' must be \
                     endpoint=namespace/name,IPv4:port,pod-uid"
                ));
            };
            if !valid_service_key(service) {
                return Err(format!(
                    "service endpoint '{line}' needs a namespace/name Service key"
                ));
            }
            if !valid_pod_uid(pod_uid) {
                return Err(format!(
                    "service endpoint '{line}' needs an RFC 4122 Pod UID"
                ));
            }
            let addr = addr
                .parse::<SocketAddrV4>()
                .map(SocketAddr::V4)
                .map_err(|_| format!("service endpoint '{line}' needs an IPv4 socket address"))?;
            let service_endpoints = endpoints.entry(service.to_string()).or_default();
            if service_endpoints.iter().any(|(known, _)| *known == addr) {
                return Err(format!("service endpoint '{line}' is duplicated"));
            }
            service_endpoints.push((addr, pod_uid.to_string()));
        } else {
            entries.push(line);
        }
    }
    let version = version.ok_or_else(|| "service target feed has no version".to_string())?;
    let targets = parse_service_targets(&entries.join(","))?;
    if let Some(service) = endpoints
        .keys()
        .find(|service| !targets.contains_key(*service))
    {
        return Err(format!("service endpoint {service} has no Service target"));
    }
    Ok((version, targets, endpoints))
}

/// Resolve feed-named Services to this node's DPU-interned compact ids.
///
/// The feed names Services; the compact ids come from the DPU's interning of
/// the topology generation. A key no generation interns yet is dropped and
/// retried once a later generation defines it — the publisher republishes.
fn resolve_named_targets(
    worker_id: c_int,
    named_targets: &NamedServiceTargets,
    named_endpoints: &NamedServiceEndpoints,
) -> (ServiceTargets, ServiceEndpoints) {
    let mut targets = HashMap::new();
    let mut endpoints = HashMap::new();
    for (key, addr) in named_targets {
        match resolve_service_key(worker_id, key) {
            Some(id) => {
                targets.insert(id, *addr);
                if let Some(eps) = named_endpoints.get(key) {
                    endpoints.insert(id, eps.clone());
                }
            }
            None => eprintln!(
                "[l7_linkerd] worker {worker_id}: Service {key} has no interned id yet; \
                 target dropped until a generation defines it"
            ),
        }
    }
    (targets, endpoints)
}

fn admin_addr_for_worker(
    mut addr: SocketAddr,
    worker_id: c_int,
    every_worker: bool,
) -> Result<SocketAddr, String> {
    if !every_worker || worker_id == 0 || addr.port() == 0 {
        return Ok(addr);
    }
    let offset = u16::try_from(worker_id)
        .map_err(|_| format!("invalid worker id for admin endpoint: {worker_id}"))?;
    let base = addr.port();
    let port = base
        .checked_add(offset)
        .ok_or_else(|| format!("admin port {base} + worker {worker_id} overflows"))?;
    addr.set_port(port);
    Ok(addr)
}

/// The process's tracing dispatcher, shared by every worker that builds a proxy.
///
/// Registration is process-wide and once-only: each worker after the first takes
/// a clone of the handle the first one installed.
#[cfg(not(test))]
fn shared_trace() -> Result<linkerd_app::trace::Handle, String> {
    static TRACE: std::sync::OnceLock<Result<linkerd_app::trace::Handle, String>> =
        std::sync::OnceLock::new();
    TRACE
        .get_or_init(|| {
            linkerd_app::trace::Settings::from_env()
                .init()
                .map_err(|e| format!("trace: {e}"))
        })
        .clone()
}

/// Build this worker's Linkerd proxy, or nothing when it carries no sessions.
///
/// `DPUMESH_L7_LINKERD_WORKER` names the worker that owns session state, or
/// `all` to give every ARM data worker its own proxy. Under `all` the data
/// plane stops funnelling L7 requests to one owner and routes them by the
/// ordinary port policy, so each worker serves the connections its ports name.
#[cfg(not(test))]
async fn build_worker(worker_id: c_int) -> Result<Option<Worker>, String> {
    let selection =
        parse_worker_selection(&std::env::var("DPUMESH_L7_LINKERD_WORKER").unwrap_or_default())?;
    let every_worker = selection == WorkerSelection::All;
    let service_targets_file = std::env::var_os("DPUMESH_L7_SERVICE_TARGETS_FILE")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .ok_or_else(|| "DPUMESH_L7_SERVICE_TARGETS_FILE is required".to_string())?;
    let document = read_feed(&service_targets_file)?;
    let (service_targets_version, named_targets, named_endpoints) =
        parse_signed_service_targets(&document)?;
    let (service_targets, service_endpoints) =
        resolve_named_targets(worker_id, &named_targets, &named_endpoints);
    let service_targets_file = Some(service_targets_file);
    let service_targets_authoritative = true;
    if let WorkerSelection::One(only) = selection {
        if worker_id != only {
            return Ok(None);
        }
    }

    linkerd_rustls::install_default_provider();

    // Initialize tracing before parsing Linkerd settings.
    let trace = shared_trace()?;

    let mut config = linkerd_app::Config::try_from_env().map_err(|e| format!("config: {e}"))?;

    // This runtime's own inbound and admin listeners are ephemeral and belong
    // to no Pod, so discovering policy for their ports would only ask the
    // controller about servers it has never heard of. The Pods this DPU is the
    // inbound enforcement point for keep full discovery: their stores are
    // built per workload from the configuration this call preserves.
    config.disable_inbound_policy_discovery();

    // The inbound, outbound and control listeners are ephemeral; the admin
    // server is the one fixed address, so it is offset by worker id. Worker 0
    // keeps the configured port. A configured port of zero is already
    // per-worker ephemeral and is left alone.
    config.admin.server.addr.0 =
        admin_addr_for_worker(config.admin.server.addr.0, worker_id, every_worker)?;

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
    // Taken before the app is consumed by spawn(): the builder outlives it and
    // is what binds one policy store to each destination Pod as it registers.
    let inbound_policies: InboundPolicyBuilder = app.dmesh_inbound_policy_builder();
    let drain = app.spawn();
    let _ = events_tx.send(DmeshEvent::InfraReady);

    let worker = Worker {
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
        service_targets,
        service_endpoints,
        service_targets_file,
        service_targets_version,
        service_targets_authoritative,
        service_targets_stamp: None,
        tx_reserve: tx_reserve_enabled(),
        counters: Counters::default(),
    };
    INBOUND.with(|slot| slot.borrow_mut().build = Some(inbound_policies));
    // The first generation is parsed here rather than adopted by a refresh, so
    // publish its placement before any session opens.
    worker.place_service_targets();
    Ok(Some(worker))
}

/// Output path selection. The reservation path copies once, into the egress
/// arena; `DMESH_L7_TX_RESERVE=0` selects the copy-then-send path instead.
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
                WORKER.with(|slot| *slot.borrow_mut() = Some(worker));
            }
            Ok(None) => {}
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

/// Verdict outcomes on the ABI. Negative values are "no verdict", which the
/// data plane resolves by the destination Service's protection class.
const VERDICT_ADMIT: c_int = 1;
const VERDICT_REFUSE: c_int = 0;
const VERDICT_NO_POLICY: c_int = -1;
const VERDICT_NOT_ATTACHED: c_int = -2;

impl InboundPolicies {
    /// Decide whether one inbound stream is admitted to a registered Pod.
    ///
    /// Two inputs decide it and a Pod supplies neither. The client address is
    /// the source Pod's signed cluster IP, because the stock evaluation matches
    /// `networks` first and an empty match denies. The identity is a TLS
    /// *state* rather than a string, because that is the only shape
    /// `Authentication::TlsAuthenticated` matches — so the verified identity is
    /// presented as an established client, exactly the substitution `DmeshIo`
    /// makes for the byte stream.
    fn verdict(&mut self, flow: &DmeshL7Flow) -> c_int {
        let Some(build) = self.build.clone() else {
            return VERDICT_NOT_ATTACHED;
        };
        let workload = flow_text(&flow.workload);
        if workload.is_empty() {
            return VERDICT_NO_POLICY;
        }
        let store = self
            .cache
            .entry(workload.clone())
            .or_insert_with(|| build(Arc::from(workload.as_str())));
        let identity = flow_text(&flow.source_identity);
        let client = SocketAddr::V4(source_addr(flow));
        let destination = SocketAddr::new(Ipv4Addr::from(flow.dst_ip).into(), flow.dst_port);
        if linkerd_app::dmesh_connection_verdict(
            store,
            destination,
            client,
            Some(identity.as_str()),
        ) {
            VERDICT_ADMIT
        } else {
            VERDICT_REFUSE
        }
    }
}

/// The destination-side admission verdict for one inbound stream.
///
/// # Safety
/// `flow` must point to a valid `struct dmesh_l7_flow`.
#[no_mangle]
pub unsafe extern "C" fn l7_inbound_verdict(worker_id: c_int, flow: *const DmeshL7Flow) -> c_int {
    if flow.is_null() {
        return VERDICT_NO_POLICY;
    }
    let flow = &*flow;
    let _ = worker_id; // the stores are this thread's
    INBOUND.with(|slot| match slot.try_borrow_mut() {
        Ok(mut policies) => policies.verdict(flow),
        // The verdict is asked from inside the data path; a nested ask would
        // be a re-entrancy this does not have, and answering "no verdict"
        // leaves the decision to the destination Service's protection class
        // rather than inventing one.
        Err(_) => VERDICT_NO_POLICY,
    })
}

/// Drop the policy watches held for a destination Pod whose registration
/// ended. The watch lifetime is the store lifetime, so this is what stops
/// them.
///
/// # Safety
/// `workload` must be NUL-terminated or null.
#[no_mangle]
pub unsafe extern "C" fn l7_inbound_forget(worker_id: c_int, workload: *const c_char) {
    if workload.is_null() {
        return;
    }
    let Ok(workload) = std::ffi::CStr::from_ptr(workload).to_str() else {
        return;
    };
    let _ = worker_id;
    INBOUND.with(|slot| {
        if let Ok(mut policies) = slot.try_borrow_mut() {
            policies.cache.remove(workload);
        }
    });
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
        while let Some(key) = w.order.last().copied() {
            w.close_session(key);
        }
        true
    });
    if !mine {
        return;
    }
    WORKER.with(|slot| {
        slot.borrow_mut().take();
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

/// Control-plane admission accounting entry point.
///
/// Registration, membership and revocation are decided on the Comch control
/// thread, which owns no worker. The counters are process-global, so every
/// worker's admin endpoint reports the same values.
///
/// # Safety
/// `kind` and `reason` must be NUL-terminated or null.
#[no_mangle]
pub unsafe extern "C" fn l7_control_event(kind: *const c_char, reason: *const c_char) {
    unsafe fn slug<'a>(text: *const c_char) -> Option<&'a str> {
        if text.is_null() {
            return None;
        }
        std::ffi::CStr::from_ptr(text).to_str().ok()
    }
    let (Some(kind), Some(reason)) = (slug(kind), slug(reason)) else {
        return;
    };
    dmesh_doca::record_control_event(kind, reason);
}

/// ABI checks for `dmesh_l7.h`.
#[cfg(test)]
mod abi {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn flow_layout_matches_c() {
        assert_eq!(size_of::<DmeshL7Flow>(), 664);
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
        assert_eq!(offset_of!(DmeshL7Flow, source_identity), 410);
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

    #[test]
    fn worker_selection_is_strict_and_supports_all() {
        assert_eq!(parse_worker_selection("").unwrap(), WorkerSelection::One(0));
        assert_eq!(
            parse_worker_selection("3").unwrap(),
            WorkerSelection::One(3)
        );
        assert_eq!(parse_worker_selection("ALL").unwrap(), WorkerSelection::All);
        assert!(parse_worker_selection("-1").is_err());
        assert!(parse_worker_selection("worker0").is_err());
    }

    #[test]
    fn all_mode_assigns_one_admin_port_per_worker() {
        let base: SocketAddr = "127.0.0.1:4191".parse().unwrap();
        assert_eq!(admin_addr_for_worker(base, 0, true).unwrap(), base);
        assert_eq!(
            admin_addr_for_worker(base, 3, true).unwrap(),
            "127.0.0.1:4194".parse().unwrap()
        );
        assert_eq!(admin_addr_for_worker(base, 3, false).unwrap(), base);

        let ephemeral: SocketAddr = "127.0.0.1:0".parse().unwrap();
        assert_eq!(
            admin_addr_for_worker(ephemeral, 3, true).unwrap(),
            ephemeral
        );
        let last: SocketAddr = "127.0.0.1:65535".parse().unwrap();
        assert!(admin_addr_for_worker(last, 1, true).is_err());
    }

    #[test]
    fn service_targets_are_strict_and_per_service() {
        let targets = parse_service_targets(
            "test-bench/echo-a=10.107.58.88:9092, test-bench/echo-b=10.111.115.171:9092",
        )
        .unwrap();
        assert_eq!(
            targets["test-bench/echo-a"],
            "10.107.58.88:9092".parse::<SocketAddrV4>().unwrap()
        );
        assert_eq!(
            targets["test-bench/echo-b"],
            "10.111.115.171:9092".parse::<SocketAddrV4>().unwrap()
        );
        assert!(parse_service_targets("test-bench/echo-a").is_err());
        // A Service is never named without its namespace; a bare name or a
        // numeric id is not a valid key.
        assert!(parse_service_targets("echo-a=10.0.0.1:80").is_err());
        assert!(parse_service_targets("11=10.0.0.1:80").is_err());
        assert!(parse_service_targets("test-bench/echo-a=[::1]:80").is_err());
        assert!(parse_service_targets(
            "test-bench/echo-a=10.0.0.1:80,test-bench/echo-a=10.0.0.2:80"
        )
        .is_err());

        let (version, targets, endpoints) = parse_versioned_service_targets(
            "# atomic controller feed\nversion=7\ntest-bench/echo-a=10.0.0.11:9092\n\
             endpoint=test-bench/echo-a,10.244.0.11:9092,11111111-1111-1111-1111-111111111111\ntest-bench/echo-b=10.0.0.20:9092\n",
        )
        .unwrap();
        assert_eq!(version, 7);
        assert_eq!(
            targets["test-bench/echo-a"],
            "10.0.0.11:9092".parse().unwrap()
        );
        // The address is what the balancer selects; the Pod UID beside it is
        // what resolves that address to a live destination.
        assert_eq!(
            endpoints["test-bench/echo-a"],
            vec![(
                "10.244.0.11:9092".parse::<SocketAddr>().unwrap(),
                "11111111-1111-1111-1111-111111111111".to_string()
            )]
        );
        assert!(parse_versioned_service_targets("test-bench/echo-a=10.0.0.11:9092").is_err());
        assert!(parse_versioned_service_targets("version=0").is_err());
        assert!(parse_versioned_service_targets(
            "version=1\nendpoint=test-bench/echo-a,10.244.0.11:9092,11111111-1111-1111-1111-111111111111\n"
        )
        .is_err());
        // An endpoint without a Pod UID, or with one that is not an RFC 4122
        // text form, resolves to nothing and is refused rather than carried.
        assert!(parse_versioned_service_targets(
            "version=1\ntest-bench/echo-a=10.0.0.11:9092\n\
             endpoint=test-bench/echo-a,10.244.0.11:9092\n"
        )
        .is_err());
        assert!(parse_versioned_service_targets(
            "version=1\ntest-bench/echo-a=10.0.0.11:9092\n\
             endpoint=test-bench/echo-a,10.244.0.11:9092,not-a-uid\n"
        )
        .is_err());
    }

    #[test]
    fn an_unsigned_service_target_feed_is_refused() {
        // The feed carries the same authority as a grant, so an unsigned or
        // badly signed generation is refused exactly like a malformed one.
        assert!(
            parse_signed_service_targets("version=4\ntest-bench/echo-a=10.0.0.11:9092\n").is_err()
        );
        assert!(parse_signed_service_targets(
            "version=4\ntest-bench/echo-a=10.0.0.11:9092\nsignature=test,forged\n"
        )
        .is_err());
        let (version, targets, _) = parse_signed_service_targets(
            "version=4\ntest-bench/echo-a=10.0.0.11:9092\nsignature=test,valid\n",
        )
        .unwrap();
        assert_eq!(version, 4);
        assert_eq!(
            targets["test-bench/echo-a"],
            "10.0.0.11:9092".parse().unwrap()
        );
        // Bytes appended after the envelope are outside the signature, so the
        // document is refused rather than parsed up to the envelope.
        assert!(parse_signed_service_targets(
            "version=4\ntest-bench/echo-a=10.0.0.11:9092\nsignature=test,valid\ntest-bench/echo-b=10.0.0.20:9092\n"
        )
        .is_err());
    }

    #[test]
    fn service_target_feed_rejects_rollback_and_applies_withdrawal() {
        let mut tw = install_worker(0);
        let path = std::env::temp_dir().join(format!(
            "dpumesh-service-targets-{}-{}",
            std::process::id(),
            session_key(1, 1)
        ));
        std::fs::write(
            &path,
            "version=2\ntest-bench/echo-a=10.0.0.11:9092\nsignature=test,valid\n",
        )
        .unwrap();
        // A generation younger than STAMP_SETTLE is always re-read, so age this
        // one to exercise the stamp itself.
        let installed = SystemTime::now() - STAMP_SETTLE * 5;
        let age = |path: &std::path::Path| {
            std::fs::OpenOptions::new()
                .write(true)
                .open(path)
                .unwrap()
                .set_modified(installed)
                .unwrap()
        };
        age(&path);
        with_test_worker(|w| {
            w.service_targets_file = Some(path.clone());
            w.service_targets_authoritative = true;
            w.refresh_service_targets().unwrap();
            assert_eq!(w.service_targets_version, 2);
        });
        // A settled generation is adopted only once: the same inode,
        // modification time and length mean no read is issued.
        std::fs::write(
            &path,
            "version=9\ntest-bench/echo-a=10.0.0.99:9092\nsignature=test,valid\n",
        )
        .unwrap();
        age(&path);
        with_test_worker(|w| {
            w.refresh_service_targets().unwrap();
            assert_eq!(w.service_targets_version, 2);
            assert_eq!(
                w.service_targets[&11],
                "10.0.0.11:9092".parse::<SocketAddrV4>().unwrap()
            );
        });
        std::fs::write(
            &path,
            "# rolled back\nversion=1\ntest-bench/echo-a=10.0.0.12:9092\nsignature=test,valid\n",
        )
        .unwrap();
        with_test_worker(|w| assert!(w.refresh_service_targets().is_err()));
        // A rejected generation is not stamped, so it keeps being rejected.
        with_test_worker(|w| assert!(w.refresh_service_targets().is_err()));
        std::fs::write(&path, "version=3\nsignature=test,valid\n").unwrap();
        with_test_worker(|w| w.refresh_service_targets().unwrap());
        let flow = request_flow(11, 1, 4001);
        assert_eq!(
            unsafe { l7_conn_open(0, session_key(1, 4001), &flow) },
            DECLINE_ERROR
        );
        assert!(tw.events.try_recv().is_err());
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn service_target_feed_refuses_an_oversized_generation() {
        let _worker = install_worker(0);
        let path = std::env::temp_dir().join(format!(
            "dpumesh-service-targets-oversized-{}",
            std::process::id()
        ));
        let mut document = String::from("version=2\n");
        while document.len() as u64 <= MAX_FEED_BYTES {
            document.push_str("# padding past the feed bound\n");
        }
        document.push_str("test-bench/echo-a=10.0.0.11:9092\nsignature=test,valid\n");
        std::fs::write(&path, &document).unwrap();
        // The startup path reads through read_feed; the refresh path checks the
        // metadata it already holds. Both refuse without reading the document.
        assert!(read_feed(&path).is_err());
        with_test_worker(|w| {
            w.service_targets_file = Some(path.clone());
            w.service_targets_authoritative = true;
            assert!(w.refresh_service_targets().is_err());
            assert_eq!(w.service_targets_version, 0);
            assert!(w.service_targets.is_empty());
        });
        std::fs::remove_file(path).unwrap();
    }

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
            service_targets: HashMap::new(),
            service_endpoints: HashMap::new(),
            service_targets_file: None,
            service_targets_version: 0,
            service_targets_authoritative: false,
            service_targets_stamp: None,
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
            // A real cluster address, because the source address is what an
            // authorization policy's networks clause is matched against.
            src_ip: u32::from(Ipv4Addr::new(10, 244, 0, (pod & 0xff) as u8)),
            dst_ip: 0,
            src_port: port,
            dst_port: port,
            src_pod: pod,
            dst_service: service,
            peer_pod: pod,
            mode: MODE_OPAQUE,
            is_reply: 0,
            workload: [0; 384],
            source_identity: [0; 254],
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
        let (io, handle) = dmesh_doca::dmesh_io_pair("10.244.0.1:1".parse().unwrap());
        tw.registrar.send(Registration { token, handle }).unwrap();
        with_test_worker(|w| w.collect_registrations());
        io
    }

    /// Take a session's channel the way the connector does: by session token,
    /// against the target Linkerd selected.
    fn take_backend(tw: &TestWorker, service: i32, conn: u64) -> DmeshIo {
        tw.backends
            .take_session(token_of(conn), service_addr(service))
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
    fn session_uses_real_policy_target_and_registered_workload() {
        let mut tw = install_worker(0);
        with_test_worker(|w| {
            w.service_targets
                .insert(21, "10.107.58.88:9092".parse().unwrap());
        });
        let mut flow = request_flow(21, 1, 4001);
        let workload = br#"{"ns":"test-bench","pod":"bench-a"}"#;
        for (dst, src) in flow.workload.iter_mut().zip(workload.iter().copied()) {
            *dst = src as c_char;
        }

        let key = session_key(1, 4001);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        match tw.events.try_recv().unwrap() {
            DmeshEvent::ConnReady(_, flow) => {
                assert_eq!(flow.dst, "10.107.58.88:9092".parse().unwrap());
                assert_eq!(flow.workload, r#"{"ns":"test-bench","pod":"bench-a"}"#);
            }
            other => panic!("expected a ready event, got {other:?}"),
        }
        assert!(tw.backends.contains_service(&service_addr(21)));
        with_test_worker(|w| w.close_session(key));
    }

    #[test]
    fn a_selected_endpoint_must_resolve_to_a_live_destination() {
        // DPUmesh chooses the backend Pod itself, so a Linkerd-selected
        // endpoint is translated rather than dialled. Every outcome that is
        // not a live local Pod declines by its own reason: a round robin or a
        // TCP fallback would carry a protected stream somewhere its policy
        // never named.
        let tw = install_worker(0);
        with_test_worker(|w| {
            w.service_targets
                .insert(21, "10.107.58.88:9092".parse().unwrap());
            w.service_endpoints.insert(
                21,
                vec![
                    (
                        "10.244.0.11:9092".parse().unwrap(),
                        "11111111-1111-1111-1111-111111111111".to_string(),
                    ),
                    (
                        "10.244.1.12:9092".parse().unwrap(),
                        "22222222-2222-2222-2222-222222222222".to_string(),
                    ),
                    (
                        "10.244.2.13:9092".parse().unwrap(),
                        "33333333-3333-3333-3333-333333333333".to_string(),
                    ),
                ],
            );
            w.place_service_targets();
        });

        let flow = request_flow(21, 1, 4001);
        let key = session_key(1, 4001);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        let token = token_of(key);

        // The session's own addresses need no endpoint resolution.
        assert!(tw.backends.take_session(token, service_addr(21)).is_ok());

        for (selected, expected) in [
            ("10.244.1.12:9092", dmesh_doca::TakeError::EndpointRemote),
            ("10.244.2.13:9092", dmesh_doca::TakeError::EndpointStale),
            (
                "10.244.9.99:9092",
                dmesh_doca::TakeError::EndpointUnresolved,
            ),
        ] {
            let flow = request_flow(21, 2, 4002);
            let key = session_key(2, 4002);
            assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
            let token = token_of(key);
            assert_eq!(
                tw.backends
                    .take_session(token, selected.parse().unwrap())
                    .err(),
                Some(expected),
                "selected {selected}"
            );
            with_test_worker(|w| w.close_session(key));
        }

        // Ports do not participate in identity: the address's IP names the
        // Pod, so a Service exposing it on another port still resolves.
        let flow = request_flow(21, 3, 4003);
        let key = session_key(3, 4003);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        let token = token_of(key);
        assert!(tw
            .backends
            .take_session(token, "10.244.0.11:8080".parse().unwrap())
            .is_ok());
        with_test_worker(|w| w.close_session(key));
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

    /// A second open on a live handle is refused. Replacing the entry would
    /// strand everything the older session owns.
    #[test]
    fn a_duplicate_live_handle_is_refused() {
        let tw = install_worker(0);
        let flow = request_flow(43, 5, 4301);
        let key = session_key(5, 4301);
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        let live = token_of(key);
        let handle = register_client(&tw, live);
        with_test_worker(|w| {
            w.sessions.get_mut(&key).unwrap().client.outstanding = vec![(0, 16)];
        });

        assert_eq!(
            unsafe { l7_conn_open(0, key, &flow) },
            DECLINE_SESSION_LIMIT
        );
        with_test_worker(|w| {
            assert_eq!(w.sessions.len(), 1);
            assert_eq!(w.sessions[&key].token, live, "the live session is intact");
            assert!(w.sessions[&key].client.handle.is_some());
            assert_eq!(w.order, vec![key], "the handle is queued once");
            assert_eq!(w.counters.connections_opened, 1);
        });
        assert_eq!(
            tw.backends.sessions_for(&service_addr(43)),
            vec![live],
            "the refused open published nothing"
        );
        assert_eq!(tw.metrics.sessions_active.get(), 1);

        // The refusal also consumed no slot, so the next handle reuses nothing.
        with_test_worker(|w| w.close_session(key));
        assert_eq!(unsafe { l7_conn_open(0, key, &flow) }, 0);
        assert_eq!(token_of(key).generation, live.generation + 1);
        with_test_worker(|w| w.close_session(key));
        assert_eq!(released(), vec![(key, 0, 16)], "released exactly once");
        drop(handle);
        assert!(tw.backends.is_empty());
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
