
### capacity

| protocol | payload | RPC/s batch→lean | CPU µs/RPC batch→lean | p50 µs batch→lean | p99 µs batch→lean | B/publication batch→lean |
|---|---:|---:|---:|---:|---:|---:|
| grpc | 64 B | 35,154→36,694 (+4.38%) | 183.01→174.92 (-4.42%) | 1,822→1,768 (-2.96%) | 3,083→2,553 (-17.19%) | 320→323 |
| grpc | 1 KiB | 33,790→34,828 (+3.07%) | 190.14→183.61 (-3.43%) | 1,878→1,829 (-2.61%) | 4,156→3,233 (-22.21%) | 2,858→2,880 |
| grpc | 8 KiB | 20,960→21,619 (+3.14%) | 314.08→305.08 (-2.87%) | 3,027→2,944 (-2.74%) | 4,777→4,407 (-7.75%) | 6,960→6,941 |
| grpc | 64 KiB | 12,679→13,120 (+3.48%) | 613.24→590.00 (-3.79%) | 5,003→4,870 (-2.66%) | 7,549→7,253 (-3.92%) | 9,678→9,690 |
| opaque | 64 B | 135,303→134,353 (-0.70%) | 23.71→23.35 (-1.51%) | 441→443 (+0.45%) | 1,103→1,116 (+1.18%) | 266→266 |
| opaque | 1 KiB | 120,658→117,052 (-2.99%) | 25.57→25.98 (+1.60%) | 483→502 (+3.93%) | 1,178→1,177 (-0.08%) | 3,455→3,454 |

RPC/s range (min-max over three runs):

| protocol | payload | batch | lean | overlap |
|---|---:|---:|---:|---|
| grpc | 64 B | 34,872–35,627 | 36,665–36,937 | no |
| grpc | 1 KiB | 33,536–34,227 | 34,638–35,227 | no |
| grpc | 8 KiB | 20,774–21,059 | 21,580–21,621 | no |
| grpc | 64 KiB | 12,645–12,713 | 13,013–13,169 | no |
| opaque | 64 B | 134,772–135,901 | 131,532–139,349 | yes |
| opaque | 1 KiB | 118,888–125,753 | 116,617–123,579 | yes |

### matched

| protocol | payload | RPC/s batch→lean | CPU µs/RPC batch→lean | p50 µs batch→lean | p99 µs batch→lean | B/publication batch→lean |
|---|---:|---:|---:|---:|---:|---:|
| grpc | 64 B | 19,999→20,000 (+0.01%) | 333.00→330.06 (-0.88%) | 998→963 (-3.51%) | 2,003→1,941 (-3.10%) | 147→144 |
| grpc | 1 KiB | 19,999→19,999 (+0.00%) | 334.75→332.06 (-0.80%) | 1,040→1,003 (-3.56%) | 2,093→2,040 (-2.53%) | 1,377→1,345 |
| grpc | 8 KiB | 10,000→9,999 (-0.01%) | 558.38→541.38 (-3.04%) | 1,704→1,695 (-0.53%) | 2,319→2,264 (-2.37%) | 4,162→4,156 |
| grpc | 64 KiB | 1,999→1,999 (+0.00%) | 1,016.25→965.00 (-5.04%) | 1,459→1,437 (-1.51%) | 2,588→2,553 (-1.35%) | 10,493→10,476 |
| opaque | 64 B | 19,999→19,999 (+0.00%) | 79.31→77.75 (-1.97%) | 263→259 (-1.52%) | 490→483 (-1.43%) | 80→80 |
| opaque | 1 KiB | 20,000→19,999 (-0.00%) | 80.81→80.69 (-0.15%) | 265→251 (-5.28%) | 491→476 (-3.05%) | 1,044→1,042 |

RPC/s range (min-max over three runs):

| protocol | payload | batch | lean | overlap |
|---|---:|---:|---:|---|
| grpc | 64 B | 19,999–19,999 | 20,000–20,000 | no |
| grpc | 1 KiB | 19,999–20,000 | 19,999–19,999 | yes |
| grpc | 8 KiB | 10,000–10,000 | 9,999–9,999 | no |
| grpc | 64 KiB | 1,999–1,999 | 1,999–2,000 | yes |
| opaque | 64 B | 19,999–20,000 | 19,999–20,000 | yes |
| opaque | 1 KiB | 19,999–20,156 | 19,999–19,999 | yes |

### drain passes and wakes per RPC (capacity, clean medians)

| protocol | payload | arm | drain calls/RPC | arms/RPC | writer wakes/RPC | retries |
|---|---:|---|---:|---:|---:|---:|
| grpc | 64 B | batch | 7.919 | 1.613 | 0.000 | 0.0 |
| grpc | 1 KiB | batch | 8.044 | 1.642 | 0.000 | 0.0 |
| grpc | 8 KiB | batch | 7.581 | 0.769 | 0.000 | 0.0 |
| grpc | 64 KiB | batch | 27.586 | 1.100 | 0.000 | 0.0 |
| opaque | 64 B | batch | 6.895 | 1.492 | 0.000 | 0.0 |
| opaque | 1 KiB | batch | 6.980 | 1.484 | 0.000 | 0.0 |
| grpc | 64 B | lean | 8.856 | 2.169 | 0.000 | 0.0 |
| grpc | 1 KiB | lean | 8.973 | 2.188 | 0.000 | 0.0 |
| grpc | 8 KiB | lean | 8.244 | 1.073 | 0.000 | 1.0 |
| grpc | 64 KiB | lean | 28.380 | 1.451 | 0.000 | 8.0 |
| opaque | 64 B | lean | 8.295 | 2.218 | 0.000 | 0.0 |
| opaque | 1 KiB | lean | 8.645 | 2.253 | 0.000 | 0.0 |

### publication shape (separate trace runs)

| protocol | payload | arm | publications | mean bytes | publications/MiB | 64 KiB full |
|---|---:|---|---:|---:|---:|---:|
| grpc | 64 B | batch | 167,291 | 320.5 | 3,271.53 | 0 |
| grpc | 64 B | lean | 194,460 | 321.9 | 3,257.12 | 0 |
| grpc | 1 KiB | batch | 167,754 | 2,879.5 | 364.15 | 0 |
| grpc | 1 KiB | lean | 194,440 | 2,872.7 | 365.02 | 0 |
| grpc | 8 KiB | batch | 325,989 | 7,006.9 | 149.65 | 0 |
| grpc | 8 KiB | lean | 339,264 | 6,999.4 | 149.81 | 0 |
| grpc | 64 KiB | batch | 850,858 | 9,490.8 | 110.48 | 0 |
| grpc | 64 KiB | lean | 916,798 | 9,508.4 | 110.28 | 0 |
| opaque | 64 B | batch | 532,479 | 266.1 | 3,941.21 | 0 |
| opaque | 64 B | lean | 536,410 | 266.2 | 3,939.44 | 0 |
| opaque | 1 KiB | batch | 507,807 | 3,449.0 | 304.02 | 0 |
| opaque | 1 KiB | lean | 511,704 | 3,451.8 | 303.78 | 0 |

### 64 KiB PMU (separate 12 s runs, middle 8 s)

| metric | batch | lean | change |
|---|---:|---:|---:|
| worker cores | 6.813 | 7.735 | +13.54% |
| cycles/RPC | 1,308,634 | 1,262,728 | -3.51% |
| instructions/RPC | 573,421 | 545,137 | -4.93% |
| IPC | 0.439 | 0.432 | -1.65% |
| cache-misses/RPC | 10,008 | 9,786 | -2.22% |
| context-switches/RPC | 0.227 | 0.195 | -14.16% |

### profile categories (exclusive self %, one 12 s window per size)

| protocol | payload | arm | copy_all_layers | atomic_helpers_all_layers | runtime | allocator | kernel | dmesh_poll_write_self | c_commit_self | other |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| grpc | 64 B | batch | 2.78 | 10.75 | 4.95 | 2.21 | 11.07 | 0.17 |  | 67.81 |
| grpc | 64 B | lean | 2.88 | 10.4 | 5.62 | 2.48 | 12.54 |  |  | 65.76 |
| grpc | 1 KiB | batch | 2.91 | 10.56 | 4.91 | 2.18 | 10.93 | 0.17 |  | 68.18 |
| grpc | 1 KiB | lean | 3.58 | 9.69 | 5.35 | 2.3 | 12.04 |  |  | 66.94 |
| grpc | 64 KiB | batch | 5.23 | 11.09 | 5.59 | 2.68 | 5.78 | 0.35 |  | 68.7 |
| grpc | 64 KiB | lean | 5.94 | 10.17 | 6.19 | 3.14 | 5.49 |  |  | 68.3 |

### top symbol movers (lean − batch, exclusive self %)


64 B losers (batch→lean):

- -0.51 (0.85→0.34) `__aarch64_cas1_rel`
- -0.47 (1.90→1.43) `h2::proto::connection::Connection<T,P,B>::poll`
- -0.35 (1.27→0.92) `core::hash::BuildHasher::hash_one`
- -0.28 (3.10→2.82) `__aarch64_ldadd8_relax`
- -0.28 (0.28→0.00) `h2::proto::streams::streams::Streams<B,P>::next_incoming`
- -0.22 (0.29→0.07) `<futures_util::future::try_future::MapErr<Fut,F> as core::future::future::Future>::poll`
- -0.22 (0.96→0.74) `__aarch64_cas1_acq`
- -0.19 (0.26→0.07) `h2::proto::streams::send::Send::recv_connection_window_update`
- -0.19 (0.32→0.13) `h2::hpack::encoder::encode_str`
- -0.19 (0.32→0.13) `h2::proto::streams::streams::StreamRef<B>::send_data`

64 B gainers:

- +0.49 (0.00→0.49) `<futures_util::future::future::Map<Fut,F> as core::future::future::Future>::poll`
- +0.26 (0.62→0.88) `_rjem_malloc`
- +0.25 (0.74→0.99) `__fget_files`
- +0.25 (0.05→0.30) `tokio::runtime::scheduler::current_thread::Context::park_internal`
- +0.24 (0.67→0.91) `<S as linkerd_stack::box_service::sealed::BoxCloneSyncService<Req,Rsp>>::box_clone_sync`
- +0.23 (0.42→0.65) `get_random_u32`
- +0.23 (0.07→0.30) `h2::share::RecvStream::is_end_stream`
- +0.22 (1.22→1.44) `tokio::runtime::task::core::Core<T,S>::poll`

1 KiB losers (batch→lean):

- -0.45 (1.00→0.55) `__aarch64_cas1_acq`
- -0.30 (1.77→1.47) `<linkerd_stack::map_err::ResponseFuture<W,F> as core::future::future::Future>::poll`
- -0.26 (0.64→0.38) `<futures_util::future::try_future::ErrInto<Fut,E> as core::future::future::Future>::poll`
- -0.25 (0.53→0.28) `<linkerd_stack::either::vendor::Either<A,B> as core::future::future::Future>::poll`
- -0.21 (0.46→0.25) `http::header::map::HeaderMap<T>::remove`
- -0.20 (0.28→0.08) `0x00000000000004bc`
- -0.20 (0.71→0.51) `__aarch64_cas1_rel`
- -0.18 (0.75→0.57) `__aarch64_swp4_rel`
- -0.17 (0.17→0.00) `<dmesh_doca::io::DmeshIo as tokio::io::async_write::AsyncWrite>::poll_write_vectored`
- -0.17 (0.17→0.00) `<http_body_util::either::Either<L,R> as http_body::Body>::poll_frame`

1 KiB gainers:

- +0.63 (2.54→3.17) `__memcpy_generic`
- +0.38 (0.67→1.05) `__fget_files`
- +0.36 (0.65→1.01) `h2::proto::connection::DynConnection<B>::recv_frame`
- +0.36 (2.04→2.40) `el0_svc_common.constprop.0`
- +0.35 (0.00→0.35) `<futures_util::future::future::Map<Fut,F> as core::future::future::Future>::poll`
- +0.31 (0.23→0.54) `ep_item_poll.isra.0`
- +0.30 (1.63→1.93) `h2::proto::connection::Connection<T,P,B>::poll`
- +0.29 (0.16→0.45) `linkerd_http_prom::body_data::metrics::RequestBodyFamilies<L>::metrics`

64 KiB losers (batch→lean):

- -0.47 (1.27→0.80) `<dmesh_l7::ExternalBackend as dmesh_doca::runtime::RuntimeBackend>::drain`
- -0.45 (1.26→0.81) `__aarch64_cas1_acq`
- -0.35 (0.35→0.00) `<dmesh_doca::io::DmeshIo as tokio::io::async_write::AsyncWrite>::poll_write_vectored`
- -0.34 (1.11→0.77) `<tracing::instrument::Instrumented<T> as core::future::future::Future>::poll`
- -0.33 (0.65→0.32) `0x00000000000004b8`
- -0.32 (1.41→1.09) `<linkerd_stack::map_err::ResponseFuture<W,F> as core::future::future::Future>::poll`
- -0.30 (0.30→0.00) `<http_body_util::either::Either<L,R> as http_body::Body>::poll_frame`
- -0.26 (1.45→1.19) `__aarch64_ldadd4_acq_rel`
- -0.24 (1.13→0.89) `__aarch64_ldadd8_rel`
- -0.21 (0.34→0.13) `drain::Watch::signaled::{{closure}}`

64 KiB gainers:

- +0.47 (4.98→5.45) `__memcpy_generic`
- +0.36 (0.00→0.36) `<futures_util::future::future::Map<Fut,F> as core::future::future::Future>::poll`
- +0.23 (0.00→0.23) `dmesh_doca::io::DmeshIo::poll_transmit`
- +0.23 (0.74→0.97) `h2::proto::streams::prioritize::Prioritize::pop_frame`
- +0.22 (0.22→0.44) `<tower::util::map_future::MapFuture<S,F> as tower_service::Service<R>>::call`
- +0.22 (0.00→0.22) `<dmesh_l7::DirectWriter as dmesh_doca::io::TxWriter>::write`
- +0.22 (0.36→0.58) `__aarch64_ldadd4_relax`
- +0.22 (0.13→0.35) `http::uri::path::PathAndQuery::from_shared`
