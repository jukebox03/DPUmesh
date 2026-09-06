# Linkerd arena TX batching

기준일: 2026-09-06

이 receipt는 working tree의 Linkerd TX ownership과 software 검증 결과를 기록한다.
BlueField 성능과 publication 분포는
[hardware receipt](../arena-tx-batching-perf-20260906/SUMMARY.md)에 있다.

## 전송 계약

각 `DmeshIo` endpoint는 최대 하나의 미게시 64 KiB arena batch를 소유한다.
scalar 및 vectored write는 accepted prefix를 C-owned arena에 직접 복사한다. Rust는
payload pointer를 보관하지 않고 token, 길이, exact route와 sealed 상태만 유지한다.
origin, local backend와 remote backend endpoint는 request connection을 공유해도
독립된 batch를 가진다.

첫 accepted write가 route를 고정한다. full batch는 다음 write 전에 게시되고,
partial batch는 worker drain에서 게시된다. `poll_flush`는 driver progress를 요청하며
HTTP/2 frame마다 publication을 강제하지 않는다. `poll_shutdown`은 DATA publication을
완료한 뒤 FIN을 허용한다.

backpressure가 발생하면 sealed batch와 accepted bytes를 보존하고 다음 driver grant에서
같은 token을 재시도한다. byte quota와 새 reservation 횟수는 epoch 단위로 제한된다.
endpoint lock과 quota lock은 C callback 전에 해제된다. abort와 connection close는
미게시 token을 취소하고 arena chunk를 반환한다.

## Correctness

- 32 B write 20회와 각 write 사이 `poll_flush`: 640 B, reservation 1회,
  publication 1회
- scalar/vectored partial prefix의 byte order 유지
- blocked flush 재시도에서 accepted payload 재복사 없음
- 취소된 `Pending` write의 caller bytes 미보존
- origin, local, remote endpoint batch의 독립 token 및 exact route 유지
- DATA publication 전 FIN 차단, publication 뒤 FIN 순서 유지
- wrong worker, handle, route와 token 거부
- abort, close와 connection 재사용에서 stale token 격리
- arena chunk 경계의 partial acceptance와 전체 chunk 반환

## Software gates

| Gate | Result |
|---|---|
| `make test` | PASS |
| Linkerd adapter Rust | 41/41 PASS |
| `dmesh-doca` | 30/30 PASS |
| non-test Rust check | PASS |
| gRPC release | 4/4 PASS |
| gRPC ASAN+UBSAN | 4/4 PASS |
| C proxy-lane ASAN+UBSAN | PASS |
| repository and submodule whitespace | PASS |

`make test`는 repository `tests/requirements.txt`로 만든 Python environment에서
실행했다. 모든 batch correctness fixture는 accepted bytes, copied bytes,
publication, reservation, retry, error와 arena 반환을 함께 검사한다.

## 관측 계약

| Metric | 의미 |
|---|---|
| `tx_arena_copy_bytes` | arena batch에 복사된 bytes |
| `tx_accepted_bytes` | DMA 또는 peer custody로 이전된 bytes |
| `tx_reserve_attempts` | 새 batch 확보 시도 |
| `tx_publications` | custody가 이전된 batch 수 |
| `tx_retries` | reserve 또는 publication 재시도 |
| `tx_budget_wait` | quota epoch를 기다린 write poll |
| `tx_writer_wakes` | 새 grant로 깨어난 writer |
| `tx_errors` | terminal TX error |

clean hardware 표본에서는 arena copy bytes와 accepted bytes가 일치했고 retry와
error는 0이었다. traffic 종료 후 arena는 1,024/1,024 free, live chunk 0이었다.
