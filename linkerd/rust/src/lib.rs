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

use std::cell::RefCell;
use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddrV4};
use std::os::raw::{c_char, c_int};

use dmesh_doca::{DmeshEvent, DmeshIoHandle, FlowId, Registration};
use tokio::sync::mpsc;

/// Mirrors `struct dmesh_l7_flow`.
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

/// The pod staging region a segment points into. DPUmesh hands the region base
/// with every segment, so the span only has to cover the largest pod buffer.
const STAGING_SPAN: usize = 64 * 1024 * 1024;

/// Bytes drained from one connection's write buffer per step.
const TX_DRAIN_MAX: usize = 64 * 1024;

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
    backend_addr: std::net::SocketAddr,
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
    /// Slot numbers name a connection to the acceptor; they are ours to issue.
    next_slot: usize,
    pending: HashMap<usize, u64>,
}

/// The client connection's handle, as DPUmesh forms it: pod in the high bits,
/// port in the low. A reply names the same pair through `peer_pod`/`dst_port`.
fn session_key(pod: i32, port: u16) -> u64 {
    ((pod as u8 as u64) << 16) | port as u64
}

thread_local! {
    static WORKER: RefCell<Option<Worker>> = const { RefCell::new(None) };
}

/// Let the runtime run whatever is ready without blocking the worker loop.
fn pump(rt: &tokio::runtime::Runtime) {
    rt.block_on(async { tokio::task::yield_now().await });
}

/// Move one endpoint's output onto the DPUmesh connection that carries it, and
/// give back custody for what it has finished reading. Both endpoints publish
/// on the request connection; `backend` is what picks the direction it travels
/// — see `Session`.
fn pump_side(
    worker_id: c_int,
    side: &mut Side,
    out_conn: Option<u64>,
    backend: i32,
) -> Result<bool, ()> {
    let Some(handle) = side.handle.as_ref() else {
        return Ok(false);
    };
    let mut did = false;
    if let Some(out) = out_conn {
        let tx = handle.take_tx(TX_DRAIN_MAX);
        if !tx.is_empty() {
            let accepted =
                unsafe { dmesh_l7_send(worker_id, out, backend, tx.as_ptr(), tx.len()) };
            if accepted < 0 {
                return Err(());
            }
            let accepted = accepted as usize;
            if accepted < tx.len() {
                handle.untake_tx(&tx[accepted..]);
            }
            if accepted > 0 {
                did = true;
            }
        }
    }
    // The whole pushed queue is consumed, so every extent it covered has been
    // read out of staging and can go back to its sender.
    if !side.outstanding.is_empty() && !handle.has_rx() {
        if let Some(conn) = side.conn {
            for (pos, len) in side.outstanding.drain(..) {
                unsafe { dmesh_l7_release(worker_id, conn, pos, len) };
            }
            did = true;
        }
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
        let mut did = false;
        let worker_id = self.id;
        let mut failed = Vec::new();
        for (&key, s) in self.sessions.iter_mut() {
            // Both endpoints publish on the request connection: what the proxy
            // writes to the backend travels onward on it, what it writes to the
            // client returns along it.
            let request = s.client.conn;
            match pump_side(worker_id, &mut s.client, request, BACKEND_ORIGIN) {
                Ok(d) => did |= d,
                Err(()) => failed.push(key),
            }
            match pump_side(worker_id, &mut s.backend, request, BACKEND_ANY) {
                Ok(d) => did |= d,
                Err(()) => failed.push(key),
            }
        }
        for key in failed {
            self.close_session(key);
        }
        did
    }

    fn close_session(&mut self, key: u64) {
        if let Some(s) = self.sessions.remove(&key) {
            if let Some(c) = s.client.conn {
                self.by_conn.remove(&c);
            }
            if let Some(c) = s.backend.conn {
                self.by_conn.remove(&c);
            }
            self.pending.remove(&s.slot);
            // Whatever the connector never took must not outlive the session.
            let _ = dmesh_doca::backend::take(&s.backend_addr);
            let _ = self.events.send(DmeshEvent::ConnClosed(s.slot));
        }
    }
}

/// Build this worker's proxy. Everything the proxy needs comes from the
/// environment, exactly as it does for the standalone binary.
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
        next_slot: 0,
        pending: HashMap::new(),
    }))
}

// ---- the contract ----

/// # Safety
/// Called once per ARM worker thread, on that thread.
#[no_mangle]
pub unsafe extern "C" fn l7_worker_attach(worker_id: c_int) -> c_int {
    linkerd_rustls::install_default_provider();
    match build_worker(worker_id) {
        Ok(Some(w)) => {
            WORKER.with(|slot| *slot.borrow_mut() = Some(w));
            eprintln!("[l7_linkerd] worker {worker_id} attached: proxy running");
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

#[no_mangle]
pub unsafe extern "C" fn l7_worker_step(_worker_id: c_int) -> c_int {
    WORKER.with(|slot| {
        let mut slot = slot.borrow_mut();
        let Some(w) = slot.as_mut() else {
            return 0;
        };
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
        return -1;
    }
    let flow = &*flow;
    if flow.mode != MODE_OPAQUE && flow.mode != MODE_FULL {
        return -1; // `decision` has no payload for the proxy to carry
    }
    WORKER.with(|slot| {
        let mut slot = slot.borrow_mut();
        let Some(w) = slot.as_mut() else {
            return -1;
        };
        // A reply belongs to a session already open: it is the same client seen
        // from the other end.
        if flow.is_reply != 0 {
            let key = session_key(flow.peer_pod, flow.dst_port);
            let Some(s) = w.sessions.get_mut(&key) else {
                return -1; // no session to attach to; forward at L4
            };
            s.backend.conn = Some(conn);
            w.by_conn.insert(conn, key);
            return 0;
        }

        let workload = {
            let bytes = &flow.workload;
            let end = bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len());
            let raw: Vec<u8> = bytes[..end].iter().map(|&c| c as u8).collect();
            String::from_utf8_lossy(&raw).into_owned()
        };
        // The proxy routes on socket addresses. DPUmesh routes on pod and
        // service identifiers, so the identifiers stand in for the addresses
        // one-for-one; the registry mapping belongs here once it is wired.
        // The range is not loopback: an outbound proxy refuses to originate a
        // connection there.
        let src = SocketAddrV4::new(
            Ipv4Addr::new(10, 97, 0, (flow.src_pod & 0xff) as u8),
            if flow.src_port == 0 { 1 } else { flow.src_port },
        );
        let dst = SocketAddrV4::new(
            Ipv4Addr::new(10, 96, 0, (flow.dst_service & 0xff) as u8),
            9092,
        );
        let slot_idx = w.next_slot;
        w.next_slot += 1;

        // The endpoint the connector reaches the backend through. Publishing it
        // is what keeps the proxy off a TCP dial: the bytes it writes here are
        // carried by this session's DPUmesh connection instead.
        let backend_addr = std::net::SocketAddr::V4(dst);
        let (backend_io, backend_handle) = dmesh_doca::dmesh_io_pair(backend_addr);
        dmesh_doca::backend::publish(backend_addr, backend_io);

        w.sessions.insert(
            conn,
            Session {
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
            },
        );
        w.by_conn.insert(conn, conn);
        w.pending.insert(slot_idx, conn);
        let ready = DmeshEvent::ConnReady(
            slot_idx,
            FlowId {
                src,
                dst,
                workload,
                is_backend: false,
            },
        );
        if w.events.send(ready).is_err() {
            eprintln!("[l7_linkerd] worker {worker_id}: acceptor gone");
            w.close_session(conn);
            return -1;
        }
        // Let the acceptor build the endpoint before the first segment lands.
        pump(&w.rt);
        w.collect_registrations();
        0
    })
}

/// # Safety
/// `base` must point to the pod staging region for the lifetime of the segment.
#[no_mangle]
pub unsafe extern "C" fn l7_conn_segment(
    _worker_id: c_int,
    conn: u64,
    base: *const u8,
    pos: u32,
    len: u32,
) -> c_int {
    if base.is_null() || len == 0 {
        return 0;
    }
    WORKER.with(|slot| {
        let mut slot = slot.borrow_mut();
        let Some(w) = slot.as_mut() else {
            return -1;
        };
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
        len as c_int
    })
}

#[no_mangle]
pub unsafe extern "C" fn l7_conn_eof(_worker_id: c_int, conn: u64) {
    WORKER.with(|slot| {
        if let Some(w) = slot.borrow_mut().as_mut() {
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
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn l7_conn_close(_worker_id: c_int, conn: u64) {
    WORKER.with(|slot| {
        if let Some(w) = slot.borrow_mut().as_mut() {
            // A session ends with its client connection; a reply closing only
            // ends that direction.
            match w.by_conn.get(&conn).copied() {
                Some(key) if key == conn => w.close_session(key),
                Some(key) => {
                    w.by_conn.remove(&conn);
                    if let Some(s) = w.sessions.get_mut(&key) {
                        s.backend.conn = None;
                    }
                }
                None => {}
            }
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn l7_worker_detach(worker_id: c_int) {
    WORKER.with(|slot| {
        if let Some(w) = slot.borrow_mut().take() {
            eprintln!(
                "[l7_linkerd] worker {worker_id} detached: {} sessions",
                w.sessions.len()
            );
        }
    });
}

/// `decision` mode is not the proxy's model: it answers by carrying a
/// connection, not by returning a verdict for one it never sees.
#[no_mangle]
pub unsafe extern "C" fn l7_resolve(
    _worker_id: c_int,
    _flow: *const DmeshL7Flow,
    _out: *mut DmeshL7Verdict,
) -> c_int {
    -1
}

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
