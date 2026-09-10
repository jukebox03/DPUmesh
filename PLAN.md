# DPUmesh 실행 계획

기준일: 2026-09-10. 현재 구현 계약은 [CONTROL](design/CONTROL.md),
[DATA](design/DATA.md), [API](design/API.md), [GRPC](design/GRPC.md)에 있다.
이 문서는 남은 작업과 향후 설계를 다룬다.

## 확정한 배치

Host와 DPU는 같은 Kubernetes cluster의 별도 node다.
Application은 host Pod, DPU runtime은 DPU DaemonSet Pod, controller는
지정 node의 Deployment Pod다. Node admin은 host systemd service이고
per-pod broker는 admin이 관리하는 host process다. Feed receiver는 DPU Pod의
sidecar다. Admin 재시작 시 broker와 연결도 종료된다. 별도 supervisor 분리와
admin 무중단 교체는 현재 요구사항이 아니다.

등록은 단일 경로다. dpumeshd가 kernel evidence, Kubernetes Pod/Service,
kubelet allocation을 확인하고 paired-DPU TLS로 등록한다. Controller grant와
membership feed, architecture mode selector는 제거했다. Controller의 topology,
Service-target feed 및 workload-scope mediation은 유지한다.

## 남은 검증과 연구

- Broker/Pod/admin/DPU 장애에 대한 반복 hardware regression과 장시간 soak.
- Worker progress와 고부하 quiescence, 성능 변동의 재현 및 원인 확인.
- 인증서 갱신 자동화와 여러 host/DPU pair에 대한 운영 확장.
- 아래의 workload mTLS, RDMA 최적화, Go 통합은 별도 계획이다. 완료된
  로컬 등록/배치 정리가 이 연구 항목의 완료를 뜻하지 않는다.

배포와 재현 명령은 [배포 문서](packaging/README-dpu-kubernetes.md)에 있다.
과거 배치 및 grant를 전제로 한 중복 구현 설명은 이 문서에서 제거했다.
아래 §11 이후의 상세 설계와 측정 계획은 보존하며, 현재 구현 여부는 실제
코드와 위의 현재 설계 문서를 기준으로 판단한다.

# 11. P2: workload mTLS를 포함한 cross-node 전송

상태: **설계 동결 대상**. 아래의 `현재(v1)`은 코드가 이미 제공하는 계약이고,
`목표(v2)`는 구현 전에 contract test로 고정할 계약이다. 배포와 실제 RDMA 장비
검토는 이 단계에서 제외한다. TCP/in-memory carrier로 증명할 수 있는 계약을 먼저
닫는다.

이 절과 기존 설계 문서가 충돌할 때 현재 구현의 설명에는 기존 설계 문서를, v2의
구현 목표에는 이 절을 적용한다. 특히 per-workload certificate를 범위 밖으로 둔
`design/CONTROL.md` 문장은 v1 설명으로만 유효하다. v2 구현 전에는 이 PLAN과
contract test fixture만 동결한다. v2 code, test와 deployment receipt가 생긴 뒤에만
`design/API.md`, `design/CONTROL.md`, `design/DATA.md`를 현재 구현 설명으로 갱신한다.

## 11.1 코드 검토 결과: 현재(v1)에 실제로 있는 것

현재 호출 경로는 다음과 같다.

~~~text
px_resolve_backend / Linkerd-selected Pod UID
  → px_peer_stream_ready
    → dmesh_peer_open                         node별 lazy open
      → dmesh_peer_transport                  TLS 1.3 session
        → peer_wire_tcp | peer_wire_rdma      message carrier
    → dmesh_peer_stream_request               STREAM_OPEN
  → dmesh_peer_stream_data_send               DATA + source custody
  → destination px_peer_{deliver,source_deliver}
    → SG-DMA → destination REV_DONE publish
  → dmesh_peer_{delivered,source_delivered}
    → STREAM_ACK
  → px_peer_release                           source custody/credit 반환
~~~

구현과 유지되는 host test로 확인된 내용:

| 항목 | 현재 계약과 근거 |
|---|---|
| carrier | `peer_wire_ops`: whole-message all-or-nothing send/recv, TCP 구현과 RDMA 구현 |
| node session | `peer_tls`: mutual TLS 1.3, self-signed Ed25519 certificate, peer raw key 추출 |
| node binding | TLS peer key를 signed topology의 node name→DPU key와 비교 |
| sharding | ARM worker마다 peer table과 carrier 하나; worker `w`는 peer base port+`w`에 연결 |
| channel | worker와 remote node당 최대 하나, lazy open, 60초 idle eviction, incarnation fence |
| stream | full-duplex handle 하나, 양쪽 allocator namespace를 owner bit로 분리 |
| routing | exact remote Pod UID와 node를 topology에서 고르고 destination이 placement를 재검증 |
| authorization | destination이 topology의 namespace/ServiceAccount/IP로 inbound verdict 계산 |
| custody | destination host RX mapping에 `REV_DONE`을 publish한 뒤 `STREAM_ACK`; 그 뒤 source credit 반환 |
| bounds | peer별 stream/open-rate/RX staging/TX inflight/extent-slot bound |
| failure | malformed/refused data frame 또는 transport fault는 node channel 전체를 synchronous reset |
| tests | peer channel, TLS tamper/key, TCP roundtrip/backpressure/loss/reuse, full-stack loopback |

현재 보안 모델은 다음과 같다.

~~~text
Pod A ─DMA─ DPU A ═ TLS(node A, node B) ═ DPU B ─DMA─ Pod B
                         │
                         └─ STREAM_OPEN의 Pod UID를 signed topology로 검증
~~~

TLS key와 certificate principal은 node/DPU다. workload identity는 TLS certificate가
아니라 node-authenticated channel 안의 claim과 signed topology lookup으로 성립한다.
따라서 v1은 **node-pair encrypted + topology-authenticated workload claim**이지,
workload/pod-to-pod mTLS가 아니다.

## 11.2 코드 검토에서 확인한 v1의 빈틈

이 목록은 “코드가 없다”와 “코드는 있으나 목표 계약과 다르다”를 구분한다.

| 빈틈 | 현재 동작 | v2에서 필요한 결정 |
|---|---|---|
| workload credential | 없음 | 양쪽 exact Pod incarnation에 묶인 certificate와 private key |
| certificate 선택 | node별 certificate 하나 | destination Pod를 handshake 전에 안전하게 선택하는 bootstrap |
| secure association | node channel 자체 | Pod pair별 workload TLS state와 cache/lifetime |
| wire ABI | C struct를 native byte order/layout으로 `memcpy` | endian·field width가 명시된 encoder/decoder와 golden vector |
| version negotiation | header version mismatch면 channel fault | signed capability와 no-downgrade rollout gate |
| `src_generation` | STREAM_OPEN에 실리지만 consumer가 읽지 않음 | v2 wire에서 제거; topology version은 검증 context로만 사용 |
| refusal detail | `destination_opened()`의 여러 실패가 `no-pod`로 합쳐짐 | identity/authz/resource/protocol reason과 failure scope 분리 |
| reset | STREAM_RESET 메시지가 없음 | stream/association/node reset을 서로 다른 범위로 정의 |
| transport ordering | 실제 TCP/TLS/RC channel은 모든 stream을 직렬화 | 보장 계약은 per-stream order, 현재 cross-stream HOL은 구현 한계로 명시 |
| send 결과 | would-block이어도 frame을 보관하고 success, 다음 call만 inflight | `TAKEN`과 `RETRY`가 custody 소유자를 정확히 표현 |
| reconnect | channel loss가 모든 stream을 poison하고 custody 해제 | 새 association/stream만 허용, 미확인 bytes 자동 replay 금지 |
| public 오류 | peer open/loss가 주로 poison/EOF로 수렴 | graceful EOF와 remote fatal error를 별도 event로 노출 |
| endpoint fan-out | 한 `px_conn`에 remote Pod pin 하나 | destination Pod별 association/stream binding |
| secret refresh | connection마다 새 traffic key지만 장수 channel 갱신 없음 | time/byte KeyUpdate 또는 bounded reconnect 계약 |
| Pod withdrawal | `px_peer_pod_gone()`이 busy writer의 `INFLIGHT`를 무시하고 retry queue가 없음; 남은 handle은 idle eviction도 막음 | association을 Pod UID에 묶고 withdrawal을 pending control queue에서 terminal 전달/reset |
| ACK 검증 | ACK range 안에서 하나 이상 찾으면 성공하여 hole/overlap/exact count를 검증하지 않음 | range 전체가 현재 outstanding set과 정확히 일치해야 retire |
| refusal 계수 | TX bound 초과를 `dmesh_peer_tx_charge()`와 caller가 같은 channel에 두 번 더할 수 있음 | refusal을 한 계층에서 peer/node/metric에 각각 정확히 한 번 기록 |

`make test`는 이 v1 계약에서 통과한다. RDMA wire test는 local RDMA address가 없어
skip된다. 이는 v1의 host contract가 보존됐다는 뜻이지 v2가 구현됐다는 뜻이 아니다.
특히 busy-writer 중 `POD_GONE`, hole/overlap ACK, refusal exact-count arm은 현재 test에
없다. 이 세 항목은 hardware 없이 재현 가능하므로 v2 작업 전에 regression test로
고정하고, v1 path를 유지하는 동안에도 방치하지 않는다.

## 11.3 동결 결정: DPU edge-terminated workload mTLS

2026-09-05 재검토 메모: 이 절은 기존 workload mTLS 설계 기준이다. **RoCEv2 IPsec
packet offload 대안은 §11.19에 별도 기록했으며, 2026-09-07 월요일 미팅에서 채택 여부를
결정한다.** 아직 이 절의 요구사항을 변경하거나 IPsec 구현을 승인한 것은 아니다.

### 왜 workload mTLS를 선택하는가

node TLS는 “어느 DPU와 연결됐는가”를 증명한다. 원격 inbound policy가 판정하는 주체는
node가 아니라 namespace/ServiceAccount와 exact Pod incarnation이다. 현재 v1도 signed
topology와 destination validation으로 일반 Pod UID claim을 제한하므로, node DPU 전체를
신뢰하는 현재 threat model에서 node TLS가 곧 insecure라는 뜻은 아니다. controller가
서명한 short-lived workload capability를 node TLS channel에 강하게 binding하는 방식도
같은 DPU-compromise boundary 안에서는 가능한 대안이다.

따라서 workload mTLS는 link confidentiality의 논리적 필수조건이나 compromised DPU에
대한 방어가 아니다. 이 계획에서 그것을 선택하는 이유는 원격 policy principal, exact
Pod credential, proof of possession, 독립 lifecycle과 traffic key를 **하나의 표준 TLS
association 결과**로 만들겠다는 요구 때문이다. 이 요구가 사라진다면 더 단순한
node-TLS+signed-capability 설계를 다시 비교해야 하며, workload TLS를 관성적으로 유지하지
않는다.

workload mTLS가 추가로 주는 보장은 다음 네 가지다.

1. controller가 승인한 `(cluster, node, Pod UID, namespace, ServiceAccount)`와 private-key
   possession을 association의 traffic key에 묶는다.
2. source와 destination 모두 exact Pod certificate를 검증한 뒤에만 policy와 DATA를
   처리하므로 confused-deputy와 cross-Pod misbinding을 fail-closed로 만든다.
3. Pod recreate, node 이동, credential rotation/withdrawal을 node의 다른 Pod session과
   분리해 끝낼 수 있다.
4. wire/log/metric의 association identity와 원격 policy principal이 같은 인증 결과에서
   나오므로 단순한 node-authenticated metadata claim보다 감사 가능성이 높다.

요구사항이 node 사이 link encryption뿐이라면 현재 node TLS로 충분하다. workload mTLS가
필요한 이유는 encryption 강도를 한 겹 더 올리기 위해서가 아니라 **per-workload 원격
authorization identity를 cryptographic association에 바인딩하기 위해서**다. 이 구조도
한 DPU가 그 node의 workload private key와 plaintext를 모두 다루므로 compromised local
DPU로부터 workload를 보호하지는 않는다. 그 위협은 명시적 non-goal이며, 이를 이유로
TLS나 key ownership을 application에 옮기지 않는다.

### 암호화 경계

application과 broker는 TLS key, TLS record 또는 ciphertext를 만들거나 해석하지 않는다.
암호화 여부는 routing이 destination node를 확정한 뒤 DPU에서만 결정한다.

~~~text
same node
Pod A TX plaintext ─DMA─ DPU ─DMA─ Pod B RX plaintext
                         (crypto 없음)

different nodes
Pod A TX plaintext ─DMA─ source DPU
                         ├─ remote route/policy 및 workload association READY 확인
                         └─ workload TLS encrypt (정확히 1회)
                                  ↓ ciphertext only
                         TCP/RDMA peer carrier
                                  ↓ ciphertext only
                     destination DPU
                         ├─ workload TLS authenticate/decrypt
                         └─ DATA/sequence 검증 ─DMA─ Pod B RX plaintext
~~~

binding 규칙:

1. host TX mapping, forward ring과 source DPU staging까지는 plaintext다. source DPU가
   remote carrier에 넘기기 직전에 workload TLS record를 만들며, `peer_wire_ops`에는
   application plaintext를 절대 넘기지 않는다.
2. destination은 carrier에서 받은 authenticated ciphertext의 tag와 sequence를 검증한
   뒤에만 plaintext를 destination staging/DMA에 공개한다. 인증 실패 데이터는 host RX
   mapping에 한 byte도 쓰지 않는다.
3. same-node path에는 workload TLS를 만들지 않는다. DPUmesh API는 application에
   plaintext byte stream을 주고받게 하며, application-side encryption을 이 보안 요구의
   구현이나 acceptance 증거로 인정하지 않는다.
4. node TLS 1.3은 static node key와 signed topology binding을 사용한 저용량
   **control lane**이다. `NODE_CAPS`, `ASSOC_OPEN`, certificate 선택, workload TLS
   handshake relay, drain/reset만 운반하며 application payload는 운반하지 않는다.
5. workload TLS 1.3은 exact Pod pair의 **data lane**이다. handshake가 `READY`가 된 뒤
   STREAM_OPEN/DATA/ACK/FIN/RESET을 운반한다. handshake record는 Pod metadata를 숨기기
   위해 node control TLS 안에서 relay하지만, READY 뒤 application-data record는 node
   TLS를 다시 통과하지 않고 carrier로 직접 나간다. 따라서 payload 암호화는 한 번이다.
6. carrier 앞의 clear envelope에는 protocol version, node-channel incarnation,
   association id, ciphertext length만 보인다. Pod UID, ServiceAccount, Service key와
   payload는 보이지 않는다. length/timing traffic analysis와 padding은 non-goal이다.
7. v1 plaintext/data frame으로 downgrade하지 않는다. 필요한 capability가 없으면
   `protocol-unsupported`로 거부한다. software crypto fallback은 TLS를 유지하는 것이며
   plaintext fallback이 아니다.
8. TCP와 RDMA는 같은 envelope, control lane, workload TLS 및 stream contract를 쓴다.
   carrier 구현에 identity, policy, certificate 또는 crypto policy를 복제하지 않는다.

정확한 claim은 **DPU edge-terminated, workload-identity mutual TLS**다. “Pod-to-Pod”는
인증 principal과 association scope를 뜻하고 TLS 실행 위치를 뜻하지 않는다.

## 11.4 Workload identity와 certificate 계약

하나의 workload credential은 policy principal과 Pod instance를 함께 인증한다.

| 이름 | 값 | 용도 |
|---|---|---|
| `policy_identity` | `<service-account>.<namespace>.serviceaccount.identity.<trust-domain>` | Linkerd policy principal과 일치 |
| `pod_uid` | Kubernetes immutable Pod UID | recreate를 구분하는 workload incarnation |
| `node_name` | canonical Kubernetes node name | 이 credential을 사용할 수 있는 DPU 범위 |
| `cluster_id` | controller가 서명한 DNS-label cluster identifier | 다른 cluster의 같은 이름 재사용 방지 |
| `credential_serial` | issuer의 nonzero unique serial | rotation과 관측 correlation |

certificate 형식은 다음으로 동결한다.

- leaf key는 DPU 안에서 생성한 ECDSA P-256 key다. private key는 DPU 밖으로 나가지
  않고 workload Pod, broker, `dpumeshd`에도 전달하지 않는다.
- SAN `dNSName`은 `policy_identity` 하나다.
- SAN `uniformResourceIdentifier`는 다음 canonical workload-instance URI 하나다.
  `dpumesh://<cluster-id>/node/<node-name>/pod/<pod-uid>/ns/<namespace>/sa/<service-account>`
  각 component는 Kubernetes/controller가 이미 검증한 canonical DNS label/subdomain 또는
  UUID text이며 percent-encoded alternative를 허용하지 않는다.
- leaf는 위 두 SAN을 정확히 하나씩 가져야 한다. wildcard, 추가 DPUmesh URI, CN fallback은
  허용하지 않는다. 별도 private X.509 OID를 만들지 않는다.
- issuer serial은 positive 63-bit integer로 제한하고 wire에서는 `u64`로 encode한다.
  zero, 음수 또는 63 bit를 넘는 serial의 leaf는 거부한다.
- issuer chain은 cluster workload trust anchor로 검증한다. self-signed workload
  certificate와 node certificate를 workload credential로 받아들이지 않는다.
- EKU는 clientAuth와 serverAuth를 모두 포함한다. source association은 client,
  destination은 server 역할을 하며 reply는 같은 full-duplex association을 쓴다.
- workload data session은 TLS 1.3만, ALPN `dpumesh-workload/2`, 양방향 certificate
  required로 고정한다. TLS 1.2, session ticket/resumption, 0-RTT,
  anonymous/PSK-only mode는 끈다. 초기 cipher suite는 software/hardware backend가
  같은 wire를 만들 수 있는 `TLS_AES_128_GCM_SHA256` 하나로 고정하고 negotiated suite와
  crypto backend를 metric에 남긴다. 다른 TLS 1.3 suite 추가는 capability, 상호운용 및
  known-answer test를 함께 추가하는 protocol 변경이다.
- certificate의 principal/instance URI가 `ASSOC_OPEN`, signed topology 또는 local live
  registration 중 하나와 다르면 association 전체를 거부한다.
- local `dma_generation`은 certificate field가 아니다. DPU 내부의 Pod slot reuse
  fence이며 association이 잡은 slot/generation과 모든 async completion에서 비교한다.

ServiceAccount가 같은 replica도 서로 다른 private key, serial, Pod instance URI를
가진다. 정책상 같은 principal일 수 있지만 cryptographic Pod instance는 같지 않다.

## 11.5 Credential issuance와 rotation 계약

`dpumesh-controller`는 registration authority(RA)다. signing key는 별도 workload
issuer가 보유하며 어떤 DPU나 `dpumeshd`도 CA private key를 갖지 않는다. RA endpoint는
inter-DPU node-control TLS와 분리된 control-plane HTTPS node-mTLS로 인증한다.

~~~text
local Pod READY
  → DPU: P-256 key + CSR 생성
  → WORKLOAD_CSR_V1 {
        request_id, node_name, pod_uid, namespace, service_account,
        cluster_id, nonce, requested_lifetime, CSR DER
    }
  → node credential로 request 서명
  → dpumeshd L4 gateway는 encrypted bytes만 relay
  → controller가 caller node key, live Pod UID/node/SA, topology를 검증
  → issuer가 leaf 발급
  → DPU가 chain/SAN/instance URI/key match를 다시 검증하고 cache publish
~~~

RA protocol은 `POST /v1/workload-certificates`다. HTTPS server identity는 configured
controller name과 별도 control-plane trust anchor로 검증하고, client는 topology에
publish된 node static key의 certificate를 제시한다. `dpumeshd` gateway는 TCP
pass-through만 한다.

request body는 최대 16 KiB의
`application/vnd.dpumesh.workload-csr-v1` binary다. 모든 integer와 string 규칙은
§11.10과 같다.

~~~text
u32 magic='DWC1', u16 version=1, u16 flags=0
bytes16 request_id, u64 issued_at_unix_s, u32 requested_lifetime_s
string cluster_id, string node_name, string pod_uid
string namespace, string service_account
bytes32 nonce
u32 csr_der_len, bytes csr_der
bytes64 node_ed25519_signature       // 앞의 모든 byte에 대한 서명
~~~

success response는 `request_id`, `credential_serial`, `not_before`, `not_after`와
각각 `u32 length + DER`인 leaf/intermediate chain을 돌려준다. refusal response는 같은
`request_id`와 stable issuer reason만 돌려주며 CSR/Pod metadata를 echo하지 않는다.
HTTP/network timeout은 terminal issuance refusal이 아니라 같은 request id로 재시도한다.
body나 chain이 64 KiB를 넘으면 protocol failure다.

권한과 실패 계약:

- RA는 caller node에 현재 배치된 live Pod만 승인한다. 다른 node의 Pod UID 요청은
  `issuer-wrong-node`다.
- issuer reason은 `issuer-wrong-node`, `issuer-pod-not-live`, `issuer-binding`,
  `issuer-replay`, `issuer-bad-csr`, `issuer-rate`, `issuer-unavailable`로 제한한다.
  앞의 다섯 개는 request terminal, `issuer-rate`는 `Retry-After` 뒤 같은 request id로,
  `issuer-unavailable`은 bounded exponential backoff로 재시도한다.
- workload의 projected bearer token을 `dpumeshd`, broker나 DPU에 전달하지 않고,
  `dpumeshd`에는 Kubernetes API bearer credential 자체를 주지 않는다.
- 동일 `request_id` 재전송은 같은 CSR에 같은 terminal result를 돌려주는 idempotent
  operation이다. 다른 CSR에 같은 id를 쓰면 protocol error다.
- CSR private-key proof, node request signature, nonce replay cache, 요청 시각/clock
  skew, 최대 lifetime을 모두 검사한다.
- 기본 leaf lifetime은 1시간, refresh 시작은 lifetime의 70%, 최소 refresh 여유는
  5분, clock skew 허용은 30초다. 값은 config가 아니라 protocol maximum 아래의
  운영 default다.
- 새 association은 가장 최근 valid credential만 사용한다. 기존 association은
  양쪽 credential 중 먼저 만료되는 시각 30초 전까지만 유지할 수 있다.
- refresh 성공 시 구 credential과 새 credential은 최대 5분 겹친다. 그 뒤 새 stream
  open을 막고 기존 association을 bounded drain한 후 reset한다.
- Pod unregister/delete, node 이동, namespace/ServiceAccount 변경은 즉시 credential을
  withdraw하고 관련 association/stream을 reset한 뒤 private key를 cleanse한다.
- issuer가 일시 중단되어도 valid credential의 기존 association은 expiry 전까지
  계속된다. 새 association은 credential 없이 열리지 않으며 plaintext/v1 fallback이 없다.
- CSR/HTTP/issuer I/O는 control thread가 담당한다. data worker는 blocking issuance를
  호출하지 않고 immutable, refcounted credential snapshot을 acquire-load한다. rotation은
  새 snapshot을 publish하고 구 snapshot은 그것을 pin한 association이 끝난 뒤 cleanse한다.

## 11.6 Association과 stream의 단위 및 소유자

association lookup key는 정확히 다음 tuple이다. destination credential serial은
source가 handshake 전에 알 필요가 없으므로 lookup input이 아니다. responder가 선택한
serial은 workload TLS 검증 뒤 association의 authenticated state에 pin한다.

~~~text
(source_pod_uid,
 destination_pod_uid,
 source_credential_serial,
 remote_node_name,
 arm_worker_id)
~~~

계약:

- association은 방향이 있다. source가 handshake initiator이며 새 application stream도
  source만 연다. destination reply는 같은 stream과 같은 full-duplex TLS를 사용한다.
  반대 방향의 새 connection은 반대 방향 association을 쓴다.
- 한 association은 exact Pod pair만 운반한다. ServiceAccount principal만 같다고 다른
  Pod UID의 stream을 합치지 않는다.
- source QP를 소유한 ARM worker가 association과 stream을 소유한다. 현재와 같이
  worker `w`의 node carrier/control channel은 peer worker `w`에 연결한다. object를 worker 사이에서
  직접 호출하지 않는다.
- v2 node pair는 같은 nonzero ARM worker count `A`를 advertise해야 한다. 다르면
  `protocol-unsupported`이고 일부 worker만 연결하지 않는다. 한 node의 carrier/credential
  초기화도 A개 전부 성공한 뒤 한 번에 publish하며 partial coverage는 rollback한다.
- association id는 node-channel incarnation 안에서 initiator가 할당한 nonzero
  64-bit 값이다. 양 node가 동시에 association을 열 수 있으므로 최상위 owner bit를
  canonical node-name order로 나누고, 나머지 63 bit는 해당 owner가 재사용하지 않는다.
  node-channel incarnation이 바뀌면 모두 stale다.
- stream id는 association 안에서 initiator가 할당한 nonzero 64-bit 값이며 재사용하지
  않는다. destination은 `STREAM_OPEN_ACK`에서 같은 id를 echo한다.
- association cache reference를 호출자가 놓아도 live stream, pending callback 또는
  custody가 있으면 파괴하지 않는다. 모두 0이고 idle timeout이 지난 뒤에만 evict한다.
- 같은 source connection이 request별로 다른 remote Pod를 고르면 각 destination Pod의
  association에서 별도 stream을 연다. `px_conn`의 단일 `peer_pod_uid` pin은
  destination-keyed pin map으로 바꾼다.
- 이 fan-out은 protocol-aware L7 connection에만 적용한다. opaque/L4 byte stream은
  첫 backend Pod 하나에 계속 pin되며 중간에 destination을 바꾸지 않는다.

## 11.7 내부 C API와 return/ownership 계약

carrier seam은 v1의 `struct peer_wire_ops`를 유지한다.

| call | contract |
|---|---|
| `connect(ctx, ip, port, &conn)` | 0이면 `conn` ownership을 caller에게 넘김; connection은 아직 pending일 수 있음 |
| `progress(ctx, accepted, max, &n)` | 실제 이동이 있으면 nonzero; pending이라는 이유만으로 nonzero가 아님; accepted connection ownership도 caller로 이동 |
| `send_msg(conn, p, n)` | 1이면 whole message를 carrier가 복사/소유, 0이면 byte 0개를 받았으므로 동일 call 재시도 가능, -1이면 terminal |
| `recv_msg(conn, p, cap)` | 양수면 whole message 하나, 0이면 empty, 음수면 terminal; partial message를 반환하지 않음 |
| `established/faulted` | read-only state; 둘 다 false인 상태는 connect pending |
| `close(conn)` | ownership을 가진 계층이 정확히 한 번 호출하는 terminal close; 뒤에 conn pointer 사용 금지 |
| `epfd(ctx)` | 전체 carrier context용 level-triggered progress hint 하나; -1이면 polling |
| `ctx_free(ctx)` | listener와 남은 connection을 닫고 context 파괴 |

`dmesh_peer_transport_new`가 성공하면 `wire_ctx` ownership을 가져가며 실패하면 caller가
계속 소유한다. transport/channel 어느 계층에서도 `send_msg(1)`을 remote delivery로
해석하지 않는다. 이 seam 아래에는 Kubernetes, Pod, certificate 또는 policy type을
추가하지 않는다.

그 위 table은 현재 v1의 `struct dmesh_peer_transport` 계약이다.

| call | contract |
|---|---|
| `connect(ctx, ip, port, prologue, n, &conn)` | 0이면 runtime이 prologue를 복사하고 conn ownership을 caller에게 넘김; negative errno면 아무 pointer/byte도 보관하지 않음 |
| `peer_key(conn, key)` | 0이면 node TLS와 prologue가 끝나 raw peer key를 반환; 음수는 아직 ready 아님이며 실제 fault는 runtime progress가 channel reset으로 보고 |
| `send(conn, p, n)` | `n`이면 plaintext 전부를 TLS가 복사해 받음, 0이면 아무 byte도 받지 않은 would-block, 음수면 terminal; partial positive는 금지 |
| `recv(conn, p, cap)` | TLS plaintext byte stream의 `1..cap` bytes, 0 empty, negative terminal; v1 frame codec가 reassemble |
| `close(conn)` | channel owner가 정확히 한 번 호출; 모든 TLS/carrier buffer를 cleanse하고 뒤의 pointer 사용 금지 |

v2에서는 이 object가 모든 payload를 감싸는 outer transport가 아니다. carrier-envelope
dispatcher, node-control TLS lane, workload TLS data lane으로 분리한다. 기존
`connect/peer_key`와 node TLS `send/recv` 의미는 control lane에 재사용할 수 있지만
workload STREAM/DATA payload가 이 `send`를 호출하는 것은 금지한다. carrier connection
ownership은 dispatcher 하나가 갖고 두 lane은 ciphertext message만 제출한다.

`peer_key<0`만 반복해서 handshake fault를 pending으로 오해하지 않도록
`dmesh_peer_transport_progress`가 deadline/carrier/TLS fault를 반드시 owner callback으로
올린다. node-control send success도 association이나 stream delivery가 아니다.

아래 이름은 목표 interface다. public API가 아니라 `dpu_proxy`와 peer subsystem 사이의
worker-local contract다. 구현 header와 contract test가 이 의미를 그대로 가져야 한다.

~~~c
enum dmesh_xfer_result {
    DMESH_XFER_TAKEN = 0,       /* 요청과 넘긴 ownership을 subsystem이 보유 */
    DMESH_XFER_PENDING = 1,     /* async setup 중; 같은 요청을 다시 제출하지 않음 */
    DMESH_XFER_RETRY = 2,       /* 아무 ownership도 받지 않음; progress 후 재시도 */
    DMESH_XFER_REFUSED = 3,     /* object-local terminal refusal */
    DMESH_XFER_FAILED = 4,      /* parent transport/association terminal fault */
};

struct dmesh_workload_id {
    char cluster_id[64];
    char pod_uid[64];
    char node_name[256];
    char namespace_name[64];
    char service_account[256];
    /* Remote destination lookup에서는 0일 수 있다. READY association은 양쪽
     * authenticated serial을 별도 state에 반드시 pin한다. */
    uint64_t credential_serial;
};

struct dmesh_local_workload_ref {
    struct dmesh_workload_id id;
    uint32_t pod_slot;
    uint32_t dma_generation;
};

struct dmesh_assoc_key {
    struct dmesh_local_workload_ref source;
    struct dmesh_workload_id destination;
    uint32_t worker_id;
};

struct dmesh_stream_spec {
    uint16_t destination_port;
    char source_service_key[128];
    char destination_service_key[128];
};

enum dmesh_xfer_result
dmesh_assoc_get(struct dmesh_peer_table *, const struct dmesh_assoc_key *,
                uint64_t request_token, struct dmesh_assoc **out,
                enum dmesh_xfer_reason *reason);
void dmesh_assoc_put(struct dmesh_assoc *);

enum dmesh_xfer_result
dmesh_stream_open(struct dmesh_assoc *, uint64_t stream_id,
                  const struct dmesh_stream_spec *, void *stream_cookie,
                  enum dmesh_xfer_reason *reason);

enum dmesh_xfer_result
dmesh_stream_send(struct dmesh_stream *, uint64_t seq,
                  const struct iovec *, size_t iovcnt,
                  uint32_t bytes, void *custody_token,
                  enum dmesh_xfer_reason *reason);

enum dmesh_xfer_result dmesh_stream_finish(struct dmesh_stream *, uint64_t final_seq,
                                           enum dmesh_xfer_reason *reason);
void dmesh_stream_reset(struct dmesh_stream *, enum dmesh_xfer_reason);
void dmesh_stream_rx_delivered(struct dmesh_stream *, uint64_t delivery_token);
void dmesh_stream_rx_failed(struct dmesh_stream *, uint64_t delivery_token,
                            enum dmesh_xfer_reason);
int dmesh_peer_progress(struct dmesh_peer_table *, int budget);
~~~

`enum dmesh_xfer_reason`의 수치도 계약이다. §11.11의 `0x0000..0x03ff` wire status는
같은 수치의 enum 값으로 사용한다. wire로 보내지 않는 local-only 값은
`wrong-worker=0x8001`, `bad-state=0x8002`, `duplicate-token=0x8003`,
`too-large=0x8004`, `bad-argument=0x8005`로 고정한다. 구현 내부 오류를 새 wire
reason으로 암묵 변환하지 않고 `FAILED/bad-state`와 local log로 처리한다.

API 규칙:

1. 모든 call과 callback은 association owner ARM worker에서만 실행한다. API 자체는
   thread-safe가 아니며 외부 lock으로 다른 worker에서 호출하는 것도 금지한다.
2. 모든 call은 synchronous `REFUSED/FAILED`일 때 `reason`을 채운다. 그 밖의 결과는
   `reason=OK`다. `assoc_get(PENDING)`과 `stream_open(PENDING)`의 terminal reason은
   각각 async callback으로만 온다.
3. `assoc_get(TAKEN)`은 READY cache hit이며 `*out`이 non-NULL이다. 새 setup을 받은
   경우 `PENDING`이고 `*out=NULL`이다. `stream_open`은 새 open을 받으면 항상
   `PENDING`이며 같은 `stream_cookie`에 opened/refused callback 중 하나만 온다.
   READY 전에 끝난 association request는 `assoc_refused`만, READY 뒤 끝난 object는
   `assoc_down`만 받는다.
4. `TAKEN`은 would-block 때문에 subsystem이 한 frame을 보관한 경우까지 포함한다.
   caller는 같은 request/data를 재제출하지 않는다.
5. `PENDING`은 `assoc_get`/`stream_open`에만 사용한다. 완료는 callback 하나로 온다.
6. `RETRY`는 subsystem이 pointer, iovec, token 어느 것도 보관하지 않았다는 뜻이다.
7. `stream_send(TAKEN)` 뒤 `custody_released(token, disposition)` callback은 정확히
   한 번 온다. `DELIVERED`는 destination REV_DONE 뒤 ACK를 받은 경우이고 `FAILED`는
   reset/transport loss로 더는 전달을 증명할 수 없는 경우다.
8. `stream_finish`는 앞선 DATA 뒤에 ordered FIN을 소유한다. `FIN_ACK` 전에는 source
   port retirement를 완료하지 않는다.
9. `stream_reset`은 idempotent다. 이후 send/finish는 `REFUSED`이고, outstanding
   custody는 각각 `FAILED`로 반환한다.
10. callback은 오직 `dmesh_peer_progress` 안에서 직렬로 호출한다. 위 API call에
   re-entrant callback을 하지 않는다. callback에서 같은 object를 reset 요청할 수
   있지만 실제 free는 현재 callback batch 뒤로 미룬다.
11. `assoc_put` 뒤 raw pointer 사용은 금지한다. live stream이 association lifetime을
   pin하고, stream close callback 뒤 stream pointer는 무효다.
12. request token, association id, stream id, custody token은 각 namespace에서
    중복될 수 없다. wrap 시 새 object 생성을 거부하고 parent를 재생성한다.
13. `dmesh_peer_progress`는 처리한 event/frame 수, idle이면 0을 반환한다. 개별
    transport fault는 callback으로 보고하고 음수로 전체 worker loop를 끝내지 않는다.
    음수는 invalid table/budget 같은 programmer error에만 사용한다.

callback contract:

~~~c
assoc_ready(request_token, assoc)
assoc_refused(request_token, reason)
assoc_down(assoc, reason)
stream_opened(stream_cookie, stream)
stream_refused(stream_cookie, reason)
stream_rx(stream, seq, bytes, len, delivery_token)
custody_released(custody_token, DELIVERED | FAILED)
peer_fin(stream, final_seq)
local_fin_acked(stream, final_seq)
stream_closed(stream)
stream_down(stream, reason)
~~~

`stream_rx`의 bytes는 callback 동안만 유효하다. callback이 `RX_HELD`를 반환하면
subsystem staging과 `delivery_token`이 유지되고 caller가
`dmesh_stream_rx_delivered(token)` 또는 `dmesh_stream_rx_failed(token)`를 정확히 한 번
호출한다. `RX_DELIVERED`면 callback 복귀 시 ACK 대상이 된다. `RX_RETRY`는 없다.
수신 후 자원이 부족하면 frame을 decrypt하기 전에 association input을 backpressure하고,
이미 받은 bytes를 무제한 별도 queue에 복사하지 않는다.

## 11.8 State machine과 teardown 계약

### Node channel

~~~text
CLOSED → CONNECTING → NODE_TLS → PROLOGUE/CAPS → OPEN
   ▲          └────────────── fault/timeout ───────┘
   └──── idle(no association/custody), rekey, topology withdrawal, fault
~~~

node channel fault는 그 위의 모든 association을 `node-channel-down`으로 끝낸다.
새 connection은 새 incarnation을 갖고 기존 object나 bytes를 이어받지 않는다.

### Workload association

~~~text
NEW → OPEN_SENT → WORKLOAD_TLS → VERIFYING → READY → DRAINING → CLOSED
 │       │            │           │         │
 └────── refusal/timeout/cert/topology/reset ┴──────────────▶ CLOSED
~~~

- `ASSOC_OPEN_ACK`는 handshake를 시작해도 된다는 admission일 뿐 authentication
  성공이 아니다.
- 양쪽 certificate chain, policy identity, Pod extension, topology placement, local
  registration generation이 모두 맞은 뒤에만 `READY`다.
- credential expiry/rotation drain에서는 새 stream을 거부하고 live stream의 bounded
  close만 기다린다. hard expiry, Pod withdrawal, node-channel loss는 즉시 reset한다.

### Application stream

~~~text
OPENING → OPEN ↔ LOCAL_FIN | PEER_FIN → BOTH_FIN → CLOSED
    └──────── protocol/authz/lifecycle fault ─────→ RESET → CLOSED
~~~

- DATA는 방향별 `seq`가 1부터 연속 증가한다. duplicate, gap, wrap은 stream reset이다.
- FIN은 `final_seq`를 포함한다. receiver가 1..final_seq를 모두 destination path에
  넘긴 뒤 EOF를 publish하고 `FIN_ACK`한다.
- 양쪽 FIN과 FIN_ACK, outstanding custody 0이 모두 성립해야 graceful close다.
- RESET은 정상 EOF가 아니다. 아직 전달 증명되지 않은 source custody는 `FAILED`다.
- stream 하나의 unknown id, bad seq, policy withdrawal은 가능하면 그 stream만 reset한다.
  frame boundary나 association TLS integrity를 믿을 수 없으면 association 전체를 reset한다.
- graceful path는 `local_fin_acked`/`peer_fin`을 필요한 방향에 각각 한 번 보낸 뒤
  `stream_closed`로 끝난다. failure path는 `stream_down` 하나로 끝나며
  `stream_closed`나 `peer_fin`을 추가로 보내지 않는다.
- parent fault callback 순서는 (1) 새 call 차단, (2) outstanding custody의 `FAILED`,
  (3) 각 `stream_down`, (4) 마지막 `assoc_down`이다. terminal callback 뒤 그 object에
  callback을 더 보내지 않는다.

## 11.9 Custody, ordering과 backpressure

source custody 전이:

~~~text
CALLER_OWNS
  ├─ stream_send(RETRY/REFUSED/FAILED) ──────────────── caller가 계속 소유
  └─ stream_send(TAKEN) → XFER_OWNS
          ├─ destination SG-DMA + REV_DONE + DELIVERY_ACK → DELIVERED callback
          └─ stream/association/node loss                 → FAILED callback
~~~

반드시 유지할 불변식:

- carrier/TLS write 성공은 delivery가 아니다.
- association/stream이 READY가 되기 전에 application이 post한 bytes는 source의 기존
  QP/staging custody에 그대로 둔다. association subsystem으로 복사하거나 workload record/carrier envelope를
  만들지 않는다. setup timeout이면 QP error를 내고 기존 proxy poison path가 그
  arrival의 custody를 한 번만 해제한다.
- destination은 host RX mapping에 payload와 REV_DONE을 publish하기 전 ACK하지 않는다.
- source는 ACK 전에 L4 arrival custody나 L7 arena chunk를 재사용하지 않는다.
- ACK는 `(association incarnation, stream_id, direction, seq range)`를 모두 확인한다.
  range 일부만 존재하는 ACK, 겹친 ACK, 이미 retire된 ACK는 protocol error다.
- reconnect는 outstanding DATA, OPEN, FIN을 자동 replay하지 않는다. application-level
  protocol이 retry할 수는 있지만 DPUmesh는 exactly-once를 주장하지 않는다.
- stream order만 public guarantee다. 한 workload association의 TLS record queue와
  현재 단일 TCP/RC carrier ordering 때문에 cross-stream head-of-line blocking이 있을
  수 있으며 이는 기능 오류가 아니라 추후 carrier/sharding 최적화 대상이다.

bound는 association, remote peer/worker, local worker global 세 단계에서 동시에 적용한다.

| bound | association | peer/worker | worker global | 초과 동작 |
|---|---:|---:|---:|---|
| live association | — | 256 | 4096 | 새 association refusal |
| concurrent handshake | — | 16 | 64 | retry-after/backpressure |
| association open rate | — | 100/s | 1000/s | rate refusal |
| live stream | 1024 | 4096 | 16384 | stream refusal |
| RX staging bytes | 4 MiB | 16 MiB | 64 MiB | workload data-lane input backpressure, timeout 시 assoc reset |
| TX un-ACKed bytes | 4 MiB | 16 MiB | 64 MiB | `stream_send(RETRY)` |
| TX/RX extent slots | 2048 | 8192 | 32768 | `stream_send(RETRY)` 또는 input backpressure |
| one DATA extent | 64 KiB | 64 KiB | — | malformed/size refusal |
| handshake bytes | 64 KiB | 1 MiB aggregate | 4 MiB aggregate | association reset |
| handshake time | 5 s | — | — | association timeout |
| RX input pause | 5 s | — | — | `staging-limit` association reset |
| oldest un-ACKed DATA | 30 s | — | — | `delivery-timeout` stream reset |
| FIN/FIN_ACK | 5 s | — | — | `fin-timeout` stream reset |
| idle association | 60 s | — | — | stream/custody 0일 때 eviction |

위 값은 public ABI가 아니지만 security/resource contract다. 변경은 양쪽 protocol
capability와 bound test를 함께 바꿔야 하며 “동적으로 무제한”인 설정은 제공하지 않는다.
별도로 한 local source Pod와 한 remote source Pod UID가 node 전체에서 가질 수 있는
합계는 association 256, live stream 4096, un-ACKed bytes 16 MiB다. accounting key는
signed/certificate-bound Pod UID이며 association을 여러 worker/peer에 나눠도 합산한다.
Pod bound 초과는 그 Pod의 새 admission만 거부한다.

## 11.10 Wire v2 계약

v1 struct wire format을 확장하지 않는다. v2는 explicit codec를 쓰고 C struct padding을
전송하지 않는다.

공통 규칙:

- 모든 integer는 unsigned network byte order다. signed enum이나 pointer를 보내지 않는다.
- string은 NUL 없는 `u16 length + canonical ASCII bytes`다. 각 field별 최대 길이를
  decode 전에 검사한다.
- unknown version/type, nonzero reserved/flag, length overflow, trailing bytes는 해당
  framing 계층의 protocol fault다.
- decoder는 header를 전부 받은 뒤 payload length를 검사하고 allocation 전에 maximum을
  적용한다. partial carrier message를 valid frame으로 처리하지 않는다.
- wire reason code는 명시적 숫자 table이며 내부 C enum 값을 그대로 serialize하지 않는다.
- golden byte vector를 x86_64와 aarch64에서 같은 값으로 검사한다.

carrier가 가장 먼저 읽는 clear envelope는 32 bytes다. 이것은 routing용이며 application
plaintext가 아니다.

~~~text
u32 magic = 0x444d5332 ('DMS2')
u16 version = 2
u16 type                         // NODE_TLS_RECORD | WORKLOAD_TLS_RECORD
u32 flags = 0
u32 payload_len
u64 node_channel_incarnation
u64 association_id              // NODE는 0, WORKLOAD는 nonzero
~~~

| code | envelope type | association id | payload |
|---:|---|---|---|
| `0x0001` | `NODE_TLS_RECORD` | `0` | node-control TLS의 complete TLSCiphertext record 하나 |
| `0x0002` | `WORKLOAD_TLS_RECORD` | nonzero | READY workload association의 complete TLSCiphertext record 하나 |

두 payload 모두 최대 16,640 bytes이며 TLS record boundary와 정확히 일치해야 한다.
envelope는 암호화되지 않으므로 Pod UID나 Service key를 넣지 않는다. receiver는
`node_channel_incarnation`과 association lookup을 먼저 확인하고, TLS authentication이
성공한 뒤에만 안쪽 frame을 신뢰한다. association id 변조는 잘못 선택된 traffic key의
tag 검증 실패가 되며 plaintext를 공개하지 않는다. WORKLOAD record를 READY 전 보내거나
NODE record에 nonzero association id를 쓰는 것은 protocol fault다.

node-control TLS plaintext는 다음 32-byte frame header를 쓴다. node TLS가 이 header와
payload를 인증하며, 한 control frame은 여러 node TLS record에 걸칠 수 있다.

~~~text
u32 magic = 0x444d4332 ('DMC2')
u16 version = 2
u16 type
u32 flags = 0
u32 payload_len
u64 node_channel_incarnation
u64 association_id              // NODE_CAPS는 0
~~~

| code | control type | 방향 | payload 의미 |
|---:|---|---|---|
| `0x0001` | `NODE_CAPS` | both | protocol min/max, mandatory feature bits, hard maximums |
| `0x0101` | `ASSOC_OPEN` | source→destination | source/destination Pod tuple, source credential serial, topology version |
| `0x0102` | `ASSOC_OPEN_ACK` | destination→source | accepted 또는 stable reason; certificate 인증 성공을 뜻하지 않음 |
| `0x0103` | `ASSOC_HANDSHAKE_RECORD` | both | workload TLS handshake record 하나; application data 금지 |
| `0x0104` | `ASSOC_DRAIN` | either | new stream 금지, deadline과 reason |
| `0x0105` | `ASSOC_RESET` | either | association terminal reason |

control payload field order:

~~~text
NODE_CAPS       u16 min_version, u16 max_version, u64 feature_bits,
                u32 max_control_payload, u32 max_associations,
                u32 max_handshakes, u16 arm_worker_count, u16 reserved
ASSOC_OPEN      workload_id source, workload_id destination,
                u64 source_credential_serial, u64 topology_version,
                u64 open_nonce
ASSOC_OPEN_ACK  u16 status, u16 reserved, u32 max_workload_plaintext,
                u64 destination_credential_serial, u64 open_nonce
ASSOC_HANDSHAKE_RECORD
                raw complete workload-TLS handshake record
ASSOC_DRAIN     u16 reason, u16 reserved, u32 drain_timeout_ms
ASSOC_RESET     u16 reason, u16 reserved, u32 reserved
~~~

`workload_id`는 §11.4의 `cluster_id`, `pod_uid`, `node_name`, namespace,
ServiceAccount를 그 순서의 length-prefixed string으로 encode한다. `ASSOC_OPEN`에는
destination credential serial을 넣지 않는다. responder가 ACK에 선택 serial을 쓰고
실제 workload TLS leaf가 같은 serial/Pod binding인지 initiator가 검증한다. control
plaintext frame maximum은 64 KiB다. handshake record는 최대 16,640 bytes이고 하나의
`ASSOC_HANDSHAKE_RECORD`에 정확히 하나만 넣는다. workload TLS가 READY가 된 뒤 나오는
application-data record는 control frame으로 감싸지 않고 `WORKLOAD_TLS_RECORD` envelope로
직접 보낸다.

workload-TLS plaintext frame header는 24 bytes다.

~~~text
u16 version = 2
u16 type
u32 flags = 0
u32 payload_len
u32 reserved = 0
u64 stream_id                  // batch/control은 0
~~~

| code | workload type | 핵심 field | 처리 시점 |
|---:|---|---|---|
| `0x0001` | `STREAM_OPEN` | destination port, source/destination Service key | policy와 local registration 확인 |
| `0x0002` | `STREAM_OPEN_ACK` | stream id, status | success 뒤에만 DATA 가능 |
| `0x0003` | `STREAM_DATA` | direction-local `u64 seq`, bytes | ordered, staging bound 안에서만 accept |
| `0x0004` | `DELIVERY_ACK` | `(stream_id, seq_first, seq_count)` batch | REV_DONE 뒤 source custody 반환 |
| `0x0005` | `STREAM_FIN` | `final_seq` | 앞선 DATA 뒤 EOF publish |
| `0x0006` | `STREAM_FIN_ACK` | `final_seq` | FIN custody와 source port retirement 허용 |
| `0x0007` | `STREAM_RESET` | stable reason | stream-local terminal failure |

workload plaintext payload field order:

~~~text
STREAM_OPEN      string source_service_key, string destination_service_key,
                 u16 destination_port, u16 reserved, u64 topology_version
STREAM_OPEN_ACK  u16 status, u16 reserved, u32 reserved
STREAM_DATA      u64 seq, raw bytes[1..65536]
DELIVERY_ACK     u16 entry_count, u16 reserved, u32 reserved,
                 repeated { u64 stream_id, u64 seq_first,
                            u32 seq_count, u32 reserved }
STREAM_FIN       u64 final_seq
STREAM_FIN_ACK   u64 final_seq
STREAM_RESET     u16 reason, u16 reserved, u32 reserved
~~~

`DELIVERY_ACK` header의 `stream_id`는 0이고 최대 entry count는 64다. 나머지 stream
frame은 header의 nonzero `stream_id`를 사용한다. `STREAM_OPEN`의 topology version도
peer hint일 뿐이며 destination은 자기 held generation으로 두 Service와 Pod binding을
검증한다.

node TLS handshake가 끝난 뒤 첫 authenticated control frame은 양쪽 모두 `NODE_CAPS`다.
CAPS 교환 전 association control 또는 `WORKLOAD_TLS_RECORD`를 보내지 않는다. topology의
signed node record에도 peer protocol min/max를 추가한다. mandatory feature 교집합이
없으면 node control lane은 열 수 있어도 workload association은 열지 않는다.
workload-mTLS-required Service에는 v1 fallback이 없다.

## 11.11 Refusal reason과 failure scope

reason은 wire, metric, log에서 같은 stable kebab-case 이름을 쓴다. 민감한 certificate
내용이나 Pod metadata는 wire reason에 넣지 않는다.

| scope | 대표 reason | 끝내는 범위 |
|---|---|---|
| node | `node-unbound`, `node-key`, `protocol-unsupported`, `node-tls`, `carrier-fault` | node channel과 모든 association |
| association | `credential-missing`, `cert-expired`, `cert-chain`, `identity-mismatch`, `pod-binding`, `topology-stale`, `assoc-rate`, `assoc-limit`, `handshake-timeout`, `assoc-protocol`, `staging-limit`, `crypto-unavailable` | association과 그 stream |
| stream open | `no-pod`, `port-mismatch`, `service-mismatch`, `policy-denied`, `no-policy`, `stream-limit` | 요청 stream만 refusal |
| stream live | `bad-sequence`, `unknown-stream`, `delivery-failed`, `peer-reset`, `fin-timeout` | 해당 stream reset |
| local API | `wrong-worker`, `bad-state`, `duplicate-token`, `too-large` | call refusal; wire에 내보내지 않음 |

wire status code는 다음 값으로 고정한다.

| code | name | scope |
|---:|---|---|
| `0x0000` | `ok` | none |
| `0x0001` | `node-unbound` | node |
| `0x0002` | `node-key` | node |
| `0x0003` | `protocol-unsupported` | node |
| `0x0004` | `node-tls` | node |
| `0x0005` | `carrier-fault` | node |
| `0x0006` | `node-protocol` | node |
| `0x0101` | `credential-missing` | association |
| `0x0102` | `cert-expired` | association |
| `0x0103` | `cert-chain` | association |
| `0x0104` | `identity-mismatch` | association |
| `0x0105` | `pod-binding` | association |
| `0x0106` | `topology-stale` | association |
| `0x0107` | `assoc-rate` | association |
| `0x0108` | `assoc-limit` | association |
| `0x0109` | `handshake-timeout` | association |
| `0x010a` | `assoc-protocol` | association |
| `0x010b` | `staging-limit` | association |
| `0x010c` | `crypto-unavailable` | association |
| `0x0201` | `no-pod` | stream open |
| `0x0202` | `port-mismatch` | stream open |
| `0x0203` | `service-mismatch` | stream open |
| `0x0204` | `policy-denied` | stream open |
| `0x0205` | `no-policy` | stream open |
| `0x0206` | `stream-limit` | stream open |
| `0x0301` | `bad-sequence` | stream live |
| `0x0302` | `unknown-stream` | stream live |
| `0x0303` | `delivery-failed` | stream live |
| `0x0304` | `peer-reset` | stream live |
| `0x0305` | `fin-timeout` | stream live |
| `0x0306` | `delivery-timeout` | stream live |

unknown nonzero status는 그것을 받은 framing 계층의 protocol fault다. local-only
programmer error는 이 table에 넣지 않으며 `enum dmesh_xfer_reason`의 별도 범위로 둔다.

scope 결정 규칙:

- carrier envelope 또는 node-control TLS integrity를 잃으면 node scope다.
- workload TLS integrity 또는 association identity를 잃으면 association scope다.
- 인증된 inner frame에서 하나의 stream만 특정할 수 있는 오류는 stream scope다.
- resource exhaustion은 다른 peer/association을 reset하지 않고 새 admission이나 input을
  backpressure한다.
- topology skew로 인한 한 stream refusal은 node를 악성으로 간주하지 않는다. 반복은
  rate limit과 metric으로 다루고 node eviction은 controller만 결정한다.

## 11.12 Public native API mapping

local/remote 선택은 application에 투명하지만 fatal과 EOF는 같지 않다. v2 구현 시
기존 event layout을 바꾸지 않고 enum과 query symbol을 additive로 추가한다.

~~~c
DMESH_EVENT_QP_ERROR = 6

typedef struct dmesh_qp_error {
    uint32_t version;
    uint32_t size;
    int32_t  sys_errno;
    uint16_t scope;       /* LOCAL, NODE, ASSOCIATION, STREAM */
    uint16_t reason;      /* stable public class; wire detail과 1:1일 필요 없음 */
    uint8_t  retryable;   /* 새 QP를 만들면 재시도 가능한가; 자동 retry라는 뜻 아님 */
    uint8_t  reserved[7];
} dmesh_qp_error_t;

int dmesh_qp_last_error(dmesh_qp_t *, dmesh_qp_error_t *);
~~~

caller는 query 전에 `version=1`, `size=sizeof(dmesh_qp_error_t)`를 설정한다. sticky
error가 있으면 함수가 나머지 field를 채우고 `0`을 반환한다. 아직 error가 없으면
`ENODATA`, NULL 인자나 지원하지 않는 version/작은 size면 `EINVAL`을 직접 반환한다.
다른 public API와 마찬가지로 `-1/errno` 방식이 아니며, destroy된 QP pointer에는
호출할 수 없다. 더 큰 `size`는 version 1 prefix만 채우고 성공해 향후 확장을 허용한다.

관측 계약:

- remote association/stream open은 QP creation 뒤 비동기로 실패할 수 있다. 이미 성공한
  `dmesh_post_send`가 ownership을 넘긴 뒤 실패하면 그 QP에 sticky
  `DMESH_EVENT_QP_ERROR`가 정확히 한 번 발생한다.
- graceful peer FIN만 `DMESH_EVENT_RECV_FIN`이다. RESET, TLS, transport, policy/open
  failure를 FIN으로 위장하지 않는다.
- `QP_ERROR` 뒤 RX buffer release와 object destruction은 여전히 가능하고, 새 TX는
  실패한다. 같은 QP를 자동 reconnect하지 않는다.
- `dmesh_qp_last_error`는 event를 받기 전후 모두 sticky snapshot을 돌려주며 QP destroy
  뒤에는 호출할 수 없다.
- public errno mapping은 `policy-denied→EACCES`, credential/key/cert mismatch→
  `EKEYREJECTED`, topology/no destination→`EHOSTUNREACH`, protocol→`EPROTO`, timeout→
  `ETIMEDOUT`, carrier/peer reset→`ECONNRESET`, `crypto-unavailable→EOPNOTSUPP`,
  local resource terminal failure→`ENOBUFS`다.
  내부 backpressure는 terminal error가 아니며 기존 `EAGAIN/TX_READY`를 쓴다.
- 기존 `DMESH_EVENT_TX_ERROR`는 local descriptor/tail submission failure 의미를 유지한다.
  remote connection fatal은 `QP_ERROR`다.
- local deferred TX fault와 remote fatal이 경합하면 먼저 latch된 terminal 원인 하나만
  이기며 `TX_ERROR` 또는 `QP_ERROR` 중 event 하나만 발생한다. 뒤 원인은 counter/log에는
  남길 수 있지만 sticky public error와 errno를 덮어쓰지 않는다.
- preload는 `QP_ERROR`를 sticky `SO_ERROR`와 blocked read/write의 errno로 변환한다.
  gRPC adapter는 policy denial을 `PERMISSION_DENIED`, 그 밖의 remote transport/
  topology/credential failure를 `UNAVAILABLE`로 끝낸다. 어느 adapter도 같은 stream을
  replay하지 않는다.

이 추가는 struct layout을 바꾸지 않는 ABI-compatible extension 후보지만, unknown event
type을 무시하는 기존 consumer의 동작을 contract test로 확인한 뒤 minor API version을
올린다. 기존 event의 의미를 바꾸는 방식으로 구현하지 않는다.

## 11.13 Topology, policy와 lifecycle 변화

| 변화 | 새 admission | 기존 association/stream |
|---|---|---|
| source/destination Pod delete 또는 node 이동 | 즉시 거부 | association 즉시 reset |
| Pod의 namespace/ServiceAccount 불일치 | credential 발급 거부 | identity mismatch로 reset |
| endpoint set에서 destination withdrawal | 새 stream 거부 | 이미 열린 stream은 기본 drain; Pod delete면 reset |
| inbound policy 변경 | 새 stream에 새 verdict | 이미 열린 stream 유지라는 현재 정책을 보존 |
| node key rotation/withdrawal | node channel 거부 | node channel reset |
| workload cert refresh | 최신 serial만 새 association | overlap 동안 유지, deadline 뒤 drain/reset |
| node-control/workload-data traffic secret boundary | 새 key 사용 | §11.14의 KeyUpdate boundary에서 갱신 또는 reset |
| carrier loss | 새 node channel 필요 | 모든 association/stream `FAILED`, replay 없음 |

association이 잡은 topology version은 identity 결정의 근거를 관측하기 위한 값이지
peer가 보낸 version을 신뢰하는 hint가 아니다. 각 DPU는 자기가 검증·adopt한 generation만
사용한다.

## 11.14 Traffic-secret refresh

node-control TLS와 workload-data TLS 모두 다음 initial policy를 쓴다. application
payload를 운반하는 것은 workload-data TLS뿐이다.

- 한 방향 ciphertext 1 GiB 또는 30분 중 먼저 도달한 때 TLS 1.3 KeyUpdate를 요청한다.
- KeyUpdate가 5초 안에 완료되지 않으면 새 stream admission을 멈추고 association을
  reset한다. node-control update 실패는 node-channel scope다.
- certificate rotation은 KeyUpdate가 아니다. certificate principal/serial이 바뀌면
  새 association을 만들고 구 association을 drain한다.
- key update 전후의 DATA/ACK sequence와 custody는 이어진다. TLS connection 자체가
  끊긴 경우에는 이어지지 않는다.
- counter는 `peer_tls_key_updates_total{layer,result}`와
  `peer_tls_ciphertext_bytes_total{layer,direction}`로 노출하고 `layer`는
  `node_control` 또는 `workload_data`다.

OpenSSL이 자동으로 갱신할 것이라고 가정하지 않는다. byte/time boundary와 failure를
fake clock 및 recording carrier test로 직접 구동한다.

## 11.15 Crypto backend와 DPU hardware acceleration

### 현재 코드와 사용 가능한 seam

현재 `peer_tls.c`는 OpenSSL `SSL`과 memory BIO로 handshake와 record crypto를 모두
software에서 수행한다. build도 `libssl/libcrypto`만 연결하며 `doca-aes-gcm`을 사용하지
않는다. 설치된 DOCA 3.1 SDK에는 async AES-GCM encrypt/decrypt task, AES-128/256 key,
IV/tag/buffer/list/task-count capability query가 있다. 이것은 **사용 가능한 후보 API**라는
뜻일 뿐 실제 BlueField device에서 task가 지원되거나 TLS와 연동됐다는 receipt는 아니다.
현재 SDK header에서 이 API는 `DOCA_EXPERIMENTAL`이므로 version compatibility도 build와
startup capability gate에서 확인한다.

가속 목표는 application이나 broker에 crypto를 옮기는 것이 아니다. X.509 parsing,
certificate verify, ECDHE/HKDF, transcript와 KeyUpdate는 검증된 TLS 1.3 library가 계속
소유하고, 대역폭을 지배하는 workload-data record의 AES-128-GCM seal/open을 DPU crypto
engine으로 보낼 수 있게 한다. node-control TLS는 저용량이므로 initial implementation에서
software여도 된다.

TLS library가 traffic secret, nonce, record sequence와 AAD를 올바르게 소유한 채 DOCA
AES-GCM을 호출할 수 있는 supported integration이 먼저 증명돼야 한다. OpenSSL provider,
library record-offload hook 또는 동등하게 review 가능한 integration만 허용한다. 가속을
위해 key-log callback으로 secret을 빼거나 TLS key schedule/record protocol을 자체 구현하지
않는다. supported integration이 없으면 software backend가 correctness 기준이고 hardware
backend 상태는 OPEN으로 남긴다.

### 내부 backend 계약

association/stream은 구체적인 OpenSSL/DOCA type을 보지 않고 다음 worker-local seam만
사용한다.

~~~c
enum dmesh_crypto_submit {
    DMESH_CRYPTO_TAKEN = 0,    /* input/cookie ownership을 backend가 가져감 */
    DMESH_CRYPTO_RETRY = 1,    /* 아무 ownership도 가져가지 않음 */
    DMESH_CRYPTO_FAILED = 2,   /* connection/backend terminal */
};

struct dmesh_workload_tls_ops {
    int  (*conn_new)(void *ctx, const struct dmesh_tls_credential *, int role,
                     void *conn_cookie, struct dmesh_workload_tls **out);
    enum dmesh_crypto_submit (*handshake_record_in)(struct dmesh_workload_tls *,
                                                    const void *, uint32_t);
    enum dmesh_crypto_submit (*seal)(struct dmesh_workload_tls *,
                                     const struct iovec *, size_t, void *record_cookie);
    enum dmesh_crypto_submit (*open)(struct dmesh_workload_tls *,
                                     const void *, uint32_t, void *record_cookie);
    int  (*key_update)(struct dmesh_workload_tls *);
    int  (*progress)(void *ctx, int budget);
    void (*close)(struct dmesh_workload_tls *);
};
~~~

handshake output, `sealed(record_cookie, tls_record)`,
`opened(record_cookie, authenticated_plaintext)`, `crypto_failed(cookie, reason)`은 owner
worker의 `progress` 안에서만 callback되고 API call에 reentrant하지 않는다. output TLS
record는 §11.10의 complete-record bound를 만족한다. initiator `conn_new`는 첫 handshake
output을 schedule하고 responder는 첫 `handshake_record_in` 전에는 output하지 않는다.
`seal` input 합계는 최대 16,384 bytes이고 성공 callback 하나가 complete TLS record 하나를
돌려준다. 더 큰 workload frame은 codec이 같은 custody를 refcount한 ordered plaintext
chunk로 나누고 receiver codec이 인증된 record plaintext를 다시 조립한다.

ownership과 security 규칙:

1. `seal(TAKEN)` 뒤 source plaintext, iovec와 custody는 `sealed` 또는 `crypto_failed`까지
   유효하다. `sealed`가 준 ciphertext를 carrier가 소유한 뒤에도 application custody는
   destination `DELIVERY_ACK`까지 유지된다.
2. `open(TAKEN)` 뒤 ciphertext buffer는 terminal callback까지 backend 소유다.
   `opened`는 AEAD tag, TLS record sequence와 content type이 모두 검증된 plaintext만 준다.
   tag 실패 시 `opened`는 절대 오지 않고 association이 `assoc-protocol`로 reset된다.
3. `RETRY`는 input/cookie를 보관하지 않는다. crypto task slot이 돌아오면 worker progress
   hint를 발생시키며 caller가 같은 operation을 재시도한다.
4. backend는 connection별 record order와 KeyUpdate barrier를 보존한다. 같은 connection의
   record `N+1`을 `N`보다 먼저 carrier/DMA에 publish하지 않는다.
5. key object는 association과 KeyUpdate epoch에 속한다. 새 epoch가 READY가 되기 전 구
   key를 파괴하지 않고, barrier 뒤 raw staging과 구 key를 cleanse/destroy한다.
6. same-node stream은 이 interface를 호출하지 않는다. remote stream도 association이
   READY가 되기 전 application plaintext로 `seal`을 호출하지 않는다.

crypto inflight bound도 custody bound에 포함한다.

| bound | association | peer/worker | worker global | 초과 동작 |
|---|---:|---:|---:|---|
| encrypt+decrypt tasks | 32 | 128 | 256 | TX `RETRY`, RX input pause |
| plaintext waiting for seal | 기존 TX un-ACKed bound에 포함 | 기존 bound에 포함 | 기존 bound에 포함 | 별도 unbounded queue 금지 |
| ciphertext waiting for open/carrier | 기존 RX/TX staging bound에 포함 | 기존 bound에 포함 | 기존 bound에 포함 | pause 또는 timeout/reset |

DOCA backend는 worker마다 독립 AES-GCM context/progress ownership을 갖고 시작할 때
encrypt/decrypt, AES-128 key, 최소 12-byte IV, 16-byte tag, max buffer/list와 max task를
query한다. 실제 task slot은 `min(device maximum, 위 worker bound)`다. DPU-local plaintext
arena와 ciphertext arena를 `doca_buf`로 등록해 사용하며, list capability가 부족한 경우에도
bounded coalesce만 허용한다. host/application memory를 crypto device에 직접 등록하거나
application thread를 completion poller로 만들지 않는다.

local DPU config key `workload_crypto_backend`은 `software`, `auto`,
`required-hardware` 세 값이며 default는 `auto`다. wire/topology가 backend를 지정하지
않고 local operator와 startup capability probe만 선택한다.

- `software`: OpenSSL correctness backend만 사용한다.
- `auto`: startup capability/integration 검증이 실패하면 software TLS를 사용하고
  `crypto_backend_fallback_total{reason}`을 올린다. TLS와 workload identity는 그대로다.
- `required-hardware`: remote association admission을 `crypto-unavailable`로 거부한다.
  same-node plaintext DMA는 영향받지 않는다.
- 한 record/task의 terminal fault면 그 association을 reset한다. device/context fault면
  해당 worker에서 그 hardware context를 쓰는 association 전부를 reset한다. 같은
  association에서 software로 바꿔 record sequence/key ownership을 이어가지 않는다.
  `auto`는 backend를 degraded로 표시한 뒤 **새 association만** software로 열 수 있다.
- 어느 mode도 plaintext 또는 v1 data frame으로 fallback하지 않는다.

software↔hardware peer는 동일한 TLS 1.3 wire이므로 서로 연결돼야 하며 backend 종류를
wire capability로 협상하지 않는다. metric은
`workload_crypto_records_total{backend,direction,result}`,
`workload_crypto_bytes_total{backend,direction}`,
`workload_crypto_inflight{worker,backend}`와 fallback counter를 제공한다. DOCA task 제출,
completion, tag failure, task exhaustion과 runtime fault의 실제 hardware 판정은 §11.18로
미루되 이 seam과 mock async backend test는 hardware 없이 먼저 닫는다.

## 11.16 Hardware 없이 먼저 닫을 contract test

새 test는 carrier→node session→association→stream→proxy adapter 순서로 쌓는다.

- [ ] v1 현재 test를 변경 없이 유지하고 v2 codec golden vector를 추가한다.
- [ ] v1 busy-writer `POD_GONE` retry, exact ACK range, refusal counter 1회 계수 회귀 시험.
- [ ] x86_64/aarch64 동일 byte order, max/min length, reserved, overflow, trailing byte.
- [ ] node-control key/name/capability mismatch와 v1 downgrade refusal.
- [ ] workload certificate 양방향 handshake와 exact Pod instance URI match.
- [ ] wrong source/destination Pod, wrong node, wrong SA, wrong cluster, expired/not-yet-valid cert.
- [ ] issuance idempotency, nonce replay, wrong-node CSR, rotation overlap과 withdrawal.
- [ ] association/stream state transition, duplicate id/token, callback non-reentrancy.
- [ ] `TAKEN/RETRY`별 pointer/token ownership과 custody callback exactly once.
- [ ] DATA order, duplicate/gap/wrap, ACK partial/overlap/unknown range, FIN/FIN_ACK ordering.
- [ ] destination REV_DONE 전에는 ACK와 source credit이 발생하지 않음.
- [ ] per-association/per-peer byte·slot·rate bound와 한 stalled peer의 격리.
- [ ] bit flip, truncation, replay를 node-control/workload-data TLS 각각에 주입하고 plaintext delivery 0 확인.
- [ ] node/association/stream 각 scope fault가 정해진 object만 끝내는지 확인.
- [ ] carrier loss 후 새 incarnation/association만 열리며 outstanding byte replay가 0인지 확인.
- [ ] destination별 pin map으로 한 HTTP/2/gRPC connection이 두 remote Pod를 안전하게 선택.
- [ ] `QP_ERROR`, preload `SO_ERROR`, gRPC status mapping contract.
- [ ] 16 KiB 이하 한 extent marker에 대해 same-node는 TLS backend call 0회, remote는
  workload `seal` 1회이고 node-control plaintext 및 carrier payload에는 marker가 한 번도
  나타나지 않음.
- [ ] workload handshake READY 전 `seal` 0회, decrypt tag verify 전 destination DMA 0 byte.
- [ ] software↔software와 software↔mock-hardware interop의 frame/custody 결과가 동일함.
- [ ] async crypto `TAKEN/RETRY`, out-of-order completion barrier, task bound와 runtime fault.
- [ ] time/byte KeyUpdate와 timeout boundary.

host-only 완료 gate:

- [ ] 모든 wire/API field, owner, return, callback, state, timeout, bound가 header와 test에 있다.
- [ ] current v1 test와 전체 `make test`가 통과한다.
- [ ] workload identity/crypto negative arm 각각 destination application delivery가 0이다.
- [ ] fault 뒤 custody token이 누락·중복 없이 전부 terminal callback을 받는다.
- [ ] plaintext와 v1 fallback이 없다는 recording-carrier receipt가 있다.

## 11.17 구현 순서

1. v1 busy `POD_GONE`, ACK exactness, refusal count regression을 추가하고 작은 fix로 닫는다.
2. v2 reason table, codec와 golden vector만 구현한다. proxy path는 바꾸지 않는다.
3. 현재 `peer_transport`를 carrier dispatcher와 node-control TLS lane으로 분리하고 v1 regression을 유지한다.
4. workload credential cache와 RA/issuer mock contract를 구현한다.
5. `dmesh_workload_tls_ops`와 software reference backend를 구현한다.
6. association state machine과 workload TLS를 in-memory carrier에서 구현한다.
7. stream v2, FIN/RESET, exact ACK/custody를 recording proxy callback에 연결한다.
8. `dpu_proxy`의 단일 pin을 destination-keyed map으로 바꾸고 L4/L7 remote path를 연결한다.
9. public `QP_ERROR`, preload, gRPC mapping을 추가한다.
10. TCP full-stack fault/rotation/churn test를 닫는다.
11. mock async backend로 task/custody contract를 닫은 뒤 supported DOCA AES-GCM
    integration을 구현한다.
12. 같은 upper contract에 RDMA carrier를 연결한다. 실제 배포와 hardware receipt는 별도다.

각 단계는 앞 단계 test를 유지한다. v1 구조를 한 번에 지우지 않고, v2 TCP fixture가
동일 payload/custody/lifecycle test를 통과한 뒤에만 proxy default를 v2로 바꾼다.

## 11.18 배포·hardware gate — 현재 작업에서는 pass

이 절은 software contract가 닫힌 뒤의 순서만 보존한다. 이번 코드/문서 검토에서는
실행하거나 성공으로 표시하지 않는다. 상세 장비 절차는 별도 로컬 운영 문서가
소유한다.

1. rapids4 관리 주소를 persistent configuration과 일치시킨다.
2. node B의 SSH/sudo/BlueField ownership을 확보한다.
3. BFB/DOCA/firmware, cable, private address, RoCE GID, ping, `rping`, `ib_send_bw`를
   양방향 확인한다.
4. Kubernetes join, host package, Device Plugin registration, node 고정 reserve,
   명시적인 workload resource, Restricted Pod admission과 node-local smoke를 닫는다.
5. canonical node map, 서로 다른 nonzero node key, protocol capability를 publish한다.
6. 두 DPU 사이 TCP로 node auth, workload mTLS, stream, refusal, loss/rotation을 먼저
   receipt한다.
7. 각 DPU에서 DOCA AES-GCM encrypt/decrypt/AES-128/12-byte-IV/16-byte-tag/task/list
   capability를 기록하고 `auto`가 실제 `doca` backend를 선택했는지 확인한다.
8. software↔DOCA와 DOCA↔DOCA workload TLS interop, tag fault, task exhaustion,
   KeyUpdate, context fault와 key destruction을 실행한다.
9. 동일 upper test를 RDMA carrier로 실행한다.
10. remote native/gRPC, bidirectional traffic, endpoint fan-out, Pod churn, peer restart,
   remote inbound policy, no-plaintext/no-fallback과 성능을 receipt한다.

최종 완료:

- [ ] 두 DPU가 expected node뿐 아니라 exact source/destination Pod incarnation과 policy
  principal을 상호 인증한다.
- [ ] remote application stream이 실제 bytes를 운반한다.
- [ ] destination REV_DONE 뒤 ACK를 받은 경우에만 source custody가 `DELIVERED`다.
- [ ] lifecycle/policy/crypto fault가 정해진 scope에서 fail-closed다.
- [ ] reconnect가 old stream/bytes를 replay하지 않는다.
- [ ] 지원되는 BlueField에서는 workload-data AES-GCM record가 DOCA backend counter로
  관측되고 application/broker crypto call은 0이며 software backend와 wire interop한다.
- [ ] TCP host receipt와 raw two-node RDMA receipt가 모두 보존된다.

## 11.19 미결정 대안: inter-node RoCEv2 + IPsec inline hardware offload

상태: **DISCUSSION ONLY / 구현·배포 미착수**. 2026-09-05 논의 내용을 보존한다.
2026-09-07 월요일 미팅에서 요구사항과 feasibility 확인 순서를 결정한다. 이 절은
§11.3~§11.18을 대체하는 확정 설계가 아니며, 현재 TLS를 끄라는 작업 지시도 아니다.
이번 작업에서는 문서만 기록하고 종료한다.

### 11.19.1 원하는 경로와 세 가지 방식의 차이

목표는 같은 node 내부 전송에는 추가 암호화를 넣지 않고, **node 밖으로 나가는 peer
traffic만 DPU/NIC의 전용 hardware에서 암호화해 RDMA로 전송**하는 것이다. 여기서
검토하는 IPsec 대상은 Ethernet/IP 기반 **RoCEv2**다. native InfiniBand나 RoCEv1에
같은 설정이 적용된다고 가정하지 않는다.

| 방식 | 인증·암호화 단위 | 암호화 위치 | 현재 상태 / 한계 |
|---|---|---|---|
| 현재 v1 node TLS + RDMA | node TLS session | DPU ARM OpenSSL, ciphertext를 verbs로 전송 | 구현됨. 전용 inline crypto offload가 아님 |
| 기존 v2 workload mTLS + DOCA AES-GCM 후보 | Pod credential별 TLS association | TLS record buffer를 async crypto에 제출한 뒤 RDMA 송신 | §11.15의 미구현 integration. buffer 기반 lookaside이지 packet inline이 아님 |
| 이번 IPsec 후보 | DPU/node 사이 IPsec SA와 packet | NIC packet pipeline에서 ESP 암·복호화 | RoCEv2 full offload 지원 근거는 있음. 우리 ARM-origin QP 경로는 미검증 |

IPsec 후보의 개념 경로:

```text
Pod A → 기존 node-local DMA/처리 → DPU A의 등록된 송신 buffer
     → NIC DMA read → NIC ESP 암호화 → 암호화된 RoCEv2 traffic
     → NIC ESP 검증·복호화 → DPU B receive buffer → 기존 DMA/처리 → Pod B
```

이는 암호화를 위해 CPU가 별도 ciphertext buffer를 만들 필요가 없는 경로의 후보다.
DMA 자체가 없어지는 것은 아니며, 현재 구현에 이미 있는 frame 조립/SEND/RECV 복사도
자동으로 사라지지 않는다. TCP용 TLS offload 설정을 verbs QP에 적용하는 방식도 아니다.

IPsec은 TLS가 아니다. IKEv2 certificate 인증으로 양쪽 node를 인증하더라도 정확한
명칭은 **node-authenticated IPsec**이지 node-to-node mTLS가 아니다. control lane에
mTLS를 유지할 수 있지만, 그것만으로 IPsec data lane이 pod-to-pod mTLS가 되지는 않는다.
문자 그대로 per-Pod TLS session이 요구사항이면 IPsec 단독 대안은 충족하지 못한다.

### 11.19.2 근거와 아직 증명되지 않은 부분

- [NVIDIA IPsec Full Offload — RDMA traffic](https://docs.nvidia.com/networking/display/mlnxofedv23102131201lts/ipsec-full-offload.pdf):
  RoCEv2와 SR-IOV VF의 full offload를 명시하며, `ip xfrm`의 `offload packet` 또는
  동등한 packet-offload 설정을 안내한다. kernel network stack을 우회하는 RDMA에는
  crypto-only offload와 full packet offload를 구분해야 한다. **우리 DPU ARM의 실제
  PF/VF/SF·QP·netdev 조합에서 된다는 실측 근거는 아직 아니다.**
- [NVIDIA DOCA East-West Overlay Encryption](https://docs.nvidia.com/doca/sdk/DOCA-East-West-Overlay-Encryption-Application/index.html):
  DPU의 strongSwan 제어와 hardware packet 암호화의 구현 참고다. host에서 OVS/VXLAN을
  통과하는 예제이므로 설정을 그대로 ARM-origin RoCE에 복사하지 않는다. 특히 예제의
  VXLAN selector와 software stack 조건은 해당 경로의 조건이지 모든 RoCE의 조건이 아니다.
- [ReDMArk, USENIX Security 2021, §7.3](https://www.usenix.org/system/files/sec21-rothenberger.pdf):
  RoCE의 IPsec 보호를 논의하는 논문 근거다. IPsec endpoint 보호와 RDMA QP의 source
  binding을 구분한다. 다른 인증된 endpoint의 QP 사칭까지 자동으로 해결한다고 읽으면
  안 된다. 이 논문은 현재 BlueField 구성의 inline 성능이나 zero-copy 완료 증거가 아니다.

따라서 “RDMA는 IPsec inline 암호화가 원천적으로 불가능하다”도, “BlueField니까 현재
코드 그대로 반드시 된다”도 결론으로 쓰지 않는다. 아래 절차는 이 근거에서 도출한
**DPUmesh용 설계 제안**이며, vendor가 DPUmesh integration을 보증한 내용이 아니다.

### 11.19.3 node 단위 보호가 충분하다고 판단할 수 있는 조건

§3.3처럼 host kernel, 등록 경로, broker, DPU와 controller/CA를 신뢰하고 일반 workload와
외부 network를 신뢰하지 않는 모델이라면, 다음을 모두 만족하는 **IPsec + workload
authorization** 조합은 검토할 수 있다. 단순히 “node끼리 암호화했으니 충분”은 근거가 아니다.

1. 외부 network의 도청·변조·replay는 인증된 peer 사이 ESP와 anti-replay가 막는다.
   unprotected ingress/egress는 차단하며 같은 node 내부 plaintext는 신뢰 경계 안에 둔다.
2. source Pod는 자기 UID/namespace/ServiceAccount를 임의로 claim하지 못한다. 신뢰된
   local registration에서 얻은 identity만 사용하고, remote에서는 controller가 승인한
   node 소유권과 exact Pod incarnation, destination policy를 검증한다.
3. workload claim은 **실제로 인증된 node와 현재 data channel**에 결합한다. 필요한 경우
   controller-signed short-lived capability를 사용하며 대상 node/service, Pod incarnation,
   expiry와 replay 범위를 명시한다. IPsec source IP만으로 Pod identity를 만들지 않는다.
4. Pod 삭제·재생성·이동, node credential 회수, policy 변경 때 기존 권한과 stream을
   철회한다. 다른 node 또는 이전 incarnation의 claim은 application delivery 전에 거부한다.
5. 한 node/DPU가 침해되면 그 node의 workload를 사칭할 수 있다는 한계를 수용한다.
   기존 DPU-terminated workload TLS도 DPU가 해당 Pod private key를 보유하므로 이 위협을
   독립적으로 해결하지는 못한다. 다른 node의 workload까지 사칭할 수 있어서는 안 된다.

위 조건은 network confidentiality/integrity와 workload authorization을 분리해 만족시키는
논거다. **per-Pod TLS proof of possession, 독립 traffic key, TLS association 단위 audit와
동일한 기능을 제공한다는 뜻은 아니다.** 이 차이를 수용하지 못하면 기존 workload mTLS
설계를 유지한다. 수용한다면 §1.2의 workload 상호 인증 invariant와 §11의 TLS-specific
계약을 명시적으로 개정해야 하며, 이 메모만으로 기존 요구사항을 완화하지 않는다.

### 11.19.4 구현 전에 닫아야 하는 hardware feasibility gate

아래는 미팅 후 승인받아 수행할 순서다. 지금 장비 설정, service restart, firmware 변경은
하지 않는다. 실제 2-node 준비 절차는 별도 로컬 운영 문서를 따른다.

1. 양쪽 장비의 crypto-enabled SKU, firmware, BFB/DOCA, kernel, mlx5 driver와 iproute2,
   IKE daemon 버전을 기록한다. 기존 장비 메모를 현재 capability 측정치로 간주하지 않는다.
2. `peer_wire_rdma.c`가 사용하는 RDMA device/port/GID와 실제 netdev·PF/VF/SF·physical
   egress를 대응시킨다. **DPU ARM process가 만든 QP의 traffic**이 packet offload 경로에
   들어가는지 확인한다. host VF의 성공이나 host-transit OVS 예제로 대신하지 않는다.
3. 그 조합의 vendor-supported full-offload recipe를 고른다. DMFS/switchdev/function
   capability, kernel/backport 조건은 해당 버전 문서로 확인한다. 서로 다른 문서의
   kernel 6.6 조건과 BlueField BFB kernel 5.15 예제를 한 가지 설치 절차로 섞지 않는다.
4. 승인된 격리 fabric에서 synthetic payload로 양방향 RoCEv2 baseline을 확보한 뒤,
   동일 ARM-origin QP 경로에 IPsec packet offload를 설정한다. test용 static SA는 packet
   처리 확인용일 뿐이며 production 인증·key lifecycle의 완료로 인정하지 않는다.
5. 양방향 SA(Security Association), ingress/egress policy, selector, replay window,
   sequence exhaustion/ESN, rekey 지원과 ESP overhead를 포함한 MTU를 확인한다. NIC의
   RDMA header/ICRC 처리까지 지원되는 경로를 사용하며 소프트웨어에서 임의 패킷 변환하지 않는다.
6. 외부 link capture와 양쪽 NIC IPsec/RDMA counter를 함께 남긴다. DPU 내부 capture는
   암호화 전/복호화 후 지점일 수 있으므로 plaintext가 보였다는 이유만으로 wire 유출로
   단정하지 않는다. 반대로 연결 성공이나 ARM CPU 감소만으로 hardware offload를 입증하지 않는다.

selector 주의: 현재 `bench/bench.sh`의 `DPUMESH_PEER_PORT` 기본값 `47900`은 peer
service/RDMA CM 설정이고 RoCEv2 wire UDP destination port `4791`과 다르다. 실제
생성되는 packet을 기준으로 RoCEv2 flow 또는 전용 DPU IP pair를 보호한다. overlay 예제의
UDP `4789`를 무조건 사용하지 않는다. CM/control packet 보호 범위도 별도로 확인한다.
여러 Pod stream이 하나의 peer/QP에 multiplex되므로 node IP pair SA가 자동으로
per-Pod SA가 되지 않는다. per-Pod SA를 원하면 별도의 flow 식별·steering 설계가 필요하다.

### 11.19.5 IPsec 제어와 fail-closed 구현 후보

첫 후보는 DPU의 privileged node service가 IKEv2와 Linux XFRM packet-offload policy를
관리하고, mesh process는 그 service의 검증된 상태를 받아 admission하는 분리 구조다.
DOCA packet/flow API 기반 별도 구현은 대안이며 동일 flow를 두 관리자가 동시에 소유하지
않는다. 구체적인 service/API 이름은 아직 정하지 않았다.

- production은 양쪽 node certificate와 명시적 peer identity 검증을 설계한다. IKE credential과
  현재 TLS node key의 관계는 controller가 승인한 mapping으로 정한다. CA private key를
  DPU에 배포하거나 TLS session secret을 임의로 IPsec key로 전용하지 않는다.
- 선택한 stack에서 `offload packet` / `hw_offload = packet` 등 **packet offload 필수**
  설정을 사용한다. 정확한 문법과 지원은 해당 stack에서 검증한다. software/crypto-only로
  내려갈 수 있는 `auto`는 이번 hardware 요구의 성공 조건으로 쓰지 않는다.
- policy가 설치되어 unprotected traffic을 차단하고, 양방향 인증·SA·실제 hardware 설치가
  확인된 뒤에만 peer DATA를 허용한다. XFRM state가 보인다는 사실만으로 끝내지 않는다.
- SA 부재/expiry, rekey 실패, daemon/NIC restart에서도 hardware/driver 경로 자체가
  plaintext를 차단해야 한다. user-space 상태 polling은 이 차단을 대신할 수 없다.
  장애 때 QP/admission을 닫고, policy 제거 순서 때문에 plaintext가 나갈 틈도 없게 한다.
- rekey 중 old/new SA overlap, anti-replay, certificate 회수, node 재시작과 connection
  incarnation의 관계를 명시한다. SPI 변경마다 application stream을 반드시 끊을 필요는
  없지만, 동일한 인증 peer와 보호 상태의 연속성을 입증하지 못하면 fail-closed한다.
- application Pod에 XFRM, SA 또는 DPU flow 변경 권한을 주지 않는다. 제어 service의 API와
  socket도 local caller를 인증한다. key material을 log나 benchmark receipt에 남기지 않는다.

IPsec 미지원·설치 실패 시 plaintext RDMA로 fallback하지 않는다. 기존 TLS를 별도 운영
모드로 유지하는 선택은 가능하지만, 그것을 hardware inline 목표 달성으로 보고하지 않는다.

### 11.19.6 실제 코드 기준 integration 변경 지점

IPsec 설정만 설치하면 현재 코드는 **TLS ciphertext를 다시 IPsec으로 암호화**한다.
아래 분리가 필요하며, `peer_tls_*` 호출만 삭제하면 인증까지 사라지므로 그렇게 수정하지 않는다.

| 현재 파일 / symbol | 현재 역할 | IPsec안을 채택했을 때 필요한 변경 |
|---|---|---|
| [doca/peer_transport.c](doca/peer_transport.c), `conn_feed`, `conn_drain`, `transport_peer_key` | 모든 carrier 위에 TLS와 node-key 인증을 제공 | carrier와 security mode를 분리. IPsec data 경로는 검증된 보호·identity binding 이후에만 plaintext frame을 carrier에 넘김 |
| [doca/peer_transport.h](doca/peer_transport.h), `peer_transport_config` | TLS seed와 wire 설정 | 인증된 node, IPsec 보호 상태, channel binding과 fault notification 계약 추가. 기존 TLS config/default 보존 |
| [doca/peer_channel.c](doca/peer_channel.c), `dmesh_peer_authenticated`, `bound_key` | TLS에서 얻은 key를 topology의 expected key와 비교 | IKE identity↔mesh node identity mapping과 실제 data channel 증명을 검증하는 별도 admission 설계 |
| [doca/peer_wire_rdma.c](doca/peer_wire_rdma.c), `rdma_send_msg`, `rdma_recv_msg` | RC SEND/RECV와 등록된 slot pool | verbs carrier는 유지. 보호 대상 device/flow를 제어 계층과 연결하고 IPsec fault 때 connection을 중단 |
| [doca/dpu_proxy.c](doca/dpu_proxy.c), remote delivery/policy 경로 | source/destination와 host DMA/custody 관리 | workload claim 검증과 incarnation 철회를 보존. IPsec이라는 이유로 destination policy나 ACK gate를 생략하지 않음 |
| [bench/bench.sh](bench/bench.sh), peer 설정 전달 | transport/bind/port 설정 | 향후 명시적인 security mode와 capability receipt를 연결. 기존 환경변수만으로 IPsec이 활성화됐다고 취급하지 않음 |

인증 seam의 핵심 미결정 사항:

1. 현재 `peer_key()`는 TLS handshake가 입증한 key를 반환한다. IPsec 경로에서 topology의
   expected key를 그냥 복사해 반환하면 **expected identity를 인증 결과로 위조**하는 셈이다.
   인증 evidence의 종류를 구분하는 API와 negative test부터 설계한다.
2. node mTLS control lane을 유지한다면 그 lane에서 승인한 양쪽 node/incarnation과
   fresh challenge를 실제 IPsec-protected data channel에 결합하는 bootstrap이 필요하다.
   data channel의 주소/QP 정보, local policy 소유권과 검증된 IKE identity를 함께 확인한다.
   metadata에 IP나 QPN을 적는 것만으로 결합이 입증되는 것은 아니다.
3. 다른 정상 인증 node의 SA/QP로 claim을 재사용하거나 old connection의 bootstrap을
   replay하지 못하게 해야 한다. bootstrap field/증명 방식/timeout/rekey 연속성은 미팅 후
   별도 protocol review와 fixture로 동결한다. 이 메모는 완성된 channel-binding protocol이 아니다.
4. DATA뿐 아니라 OPEN/ACK/FIN/RESET 등 data channel 제어 frame도 보호한다. 별도 control
   mTLS의 소량 이중 암호화는 허용 여부를 정하되, bulk payload에서 TLS를 제거할지는 요구사항
   결정 뒤에만 바꾼다. remote direct WRITE로 host memory를 노출하는 변경은 포함하지 않는다.

### 11.19.7 memcpy와 성능 검증은 별도 항목

현재 `dmesh_peer_stream_data_send()`는 payload를 `channel->tx_frame`으로 복사하고,
`rdma_send_msg()`는 다시 registered SEND slot으로 `memcpy()`한다. `rdma_recv_msg()`도
receive slot에서 caller buffer로 복사한다. TLS 경로에는 memory BIO와 `c->in/out` staging이
추가된다. 따라서 **IPsec inline 도입 = 현재 RDMA 경로 전체 zero-copy**가 아니다.

IPsec data mode가 채택되면 TLS-specific staging 제거를 검토할 수 있다. frame/SEND 복사
제거는 registered buffer 소유권과 scatter/gather, RX 복사 제거는 receive buffer lease와
repost 시점까지 바꾸는 독립 작업이다. 현재 QP의 `max_send_sge = 1`도 고려해야 한다.
local SEND CQ 완료 전에 buffer를 재사용하지 않으며, source custody의 `DELIVERED`는 여전히
destination `REV_DONE` 뒤 `STREAM_ACK` 수신으로만 확정한다. SEND CQ는 전달 ACK가 아니다.

공통 RDMA buffer API와 현재 TLS 연결의 구체적인 복사 제거 절차는 §11.20에 기록한다.
§13.4의 Linkerd TX 중간 복사 제거 계획은 그대로 유지하며, 이 IPsec 검토와 별도로 평가한다.
IPsec 또는 복사 제거만으로 성능 향상률을 사전에 약속하지 않는다.

향후 검증 순서:

1. host-only fixture: 보호 미완료 admission, 다른 node/Pod claim, bootstrap replay,
   policy 철회, SA fault 때 delivery 0과 custody terminal 처리를 검증한다. mock의 성공은
   hardware pass가 아니다.
2. 2-DPU synthetic traffic: 실제 ARM-origin QP에서 plaintext baseline 대비 IPsec packet
   offload의 wire/counter receipt를 확보한다. plaintext baseline은 격리된 test에 한정한다.
3. production 인증 후보: certificate 기반 IKE, rekey/expiry/회수, daemon/NIC restart,
   잘못된 SA와 plaintext injection 때 유출·application delivery가 없는지 확인한다.
4. mesh 연결: node binding, Pod churn과 destination policy를 유지하면서 양방향 L4/L7
   traffic을 검증한다. 현재 TLS 경로와 동일 payload·concurrency·MTU 조건에서 throughput,
   p50/p99 latency, ARM CPU, memory bandwidth, packet/drop/replay counter를 기록한다.
5. 복사 최적화는 별도 전후 결과로 분리한다. workload TLS안과 node IPsec안은 인증 단위가
   다르므로 성능 수치만으로 “동일한 보안에 더 빠름”이라고 결론 내리지 않는다.

### 11.19.8 2026-09-07 월요일 미팅 결정 항목

- [ ] 요구사항은 실제 per-Pod TLS association인가, 아니면 workload authorization을
  보존한 node 경계 암호화인가? IPsec을 mTLS라고 부르지 않는 용어에도 합의한다.
- [ ] §11.19.3의 신뢰 경계와 보안상 차이를 수용하는가? capability/channel binding의
  검증 책임과 credential 발급·철회 주체는 누구인가?
- [ ] ARM-origin RoCEv2 full-offload feasibility 실험을 진행할 것인가? 양쪽 장비 접근,
  설정 변경 권한, 복구 방법과 담당자를 정한다.
- [ ] node pair SA로 충분한가? per-Pod key isolation이 필요하면 기존 workload TLS 또는
  별도 per-Pod flow 설계를 유지해야 한다.
- [ ] full-offload 미지원이면 기존 TLS를 유지할 것인가, 장비/경로를 바꿀 것인가?
  software fallback을 hardware 목표 달성으로 인정하지 않는다.
- [ ] 채택 후에만 §1.2·§11.3~§11.18의 요구사항/구현 순서를 일관되게 개정한다.
  미채택이면 본 절은 검토 기록으로 남기고 기존 workload mTLS 계획을 유지한다.

---

## 11.20 RDMA 중간 복사 제거 — 공통 buffer lease와 TLS 전용 연결

상태: **A 공통 TX/RX lease 구현·software 회귀 완료 / B TLS 연결 미착수**.
A는 [direct-tx-rdma-20260905](bench/report/data/direct-tx-rdma-20260905/SUMMARY.md)에서
배포·측정했다. 실제 두 DPU fabric gate는 계속 별도다. 2026-09-05 실제 코드를
기준으로 작성했다. §11.19의 IPsec 채택 결정과 독립적으로 구현할 수 있다. 현재 TLS 인증과
wire protocol을 유지한다. A의 API/type은 구현됐으며, B의 TLS 직접 연결은 아래 설계로 남는다.

### 11.20.1 무엇을 없애고 무엇은 유지하는가

BIO는 OpenSSL의 입출력 인터페이스이며, 현재 `peer_tls.c`는 memory BIO를 사용한다.
송신 암호문을 출력 BIO에서 꺼내고 수신 암호문을 입력 BIO에 넣는 것은 mesh 코드의 역할이다.
RDMA는 여기서 RC QP의 SEND/RECV를 사용하며, remote WRITE로 host memory에 직접 쓰는
구현이 아니다. 다음 두 복사는 이 carrier와 TLS 사이에 있는 **중간 복사**다.

```text
현재 TX: TLS 출력 BIO → c->out → memcpy → registered SEND slot → NIC
개선 TX: TLS 출력 BIO ─────────────────→ registered SEND slot → NIC

현재 RX: NIC → registered RECV slot → memcpy → c->in → TLS 입력 BIO
개선 RX: NIC → registered RECV slot ────────────────→ TLS 입력 BIO
```

작업을 두 층으로 나눈다. 두 층을 모두 연결했을 때 위 복사가 없어지는 것이며, 각각의
효과를 더해서 “TX 두 번, RX 두 번 제거”로 세지 않는다.

| 작업 | 하는 일 | TLS/IPsec 관계 |
|---|---|---|
| A. 공통 기반 | TX slot을 빌려 채우고 제출하는 reserve/commit, RX slot을 빌려 읽고 반환하는 acquire/release | 암호화 방식과 무관. carrier는 bytes와 lifetime만 관리 |
| B. TLS 전용 연결 | `peer_tls_out/in()`이 A의 slot을 직접 사용하도록 transport pump 변경 | 현재 TLS용 adapter. data TLS를 IPsec으로 대체하면 이 부분만 다른 producer/consumer로 교체 |

제외 범위: `channel->tx_frame`의 payload 조립 복사, OpenSSL 내부 record/BIO 복사,
proxy delivery arena 복사, host↔DPU DMA, §13.4의 Linkerd TX 작업, IPsec 설정 및
새 custom BIO/crypto backend. RC SEND/RECV와 현재 `max_send_sge = 1`을 유지한다.
SGE는 NIC에 전달하는 buffer 주소·길이·등록 key이고, 이번에는 기존 contiguous slot 하나면
충분하다. 전체 경로 zero-copy나 encryption cost 제거로 표현하지 않는다.

### 11.20.2 먼저 읽을 코드와 변경 위치

아래 순서로 읽은 뒤 수정한다. line number보다 symbol을 기준으로 찾는다.

| 파일 / symbol | 현재 동작 | 필요한 변경 |
|---|---|---|
| [doca/peer_wire.h](doca/peer_wire.h), `peer_wire_ops` | copy-in `send_msg`, copy-out `recv_msg`만 제공 | optional lease callback과 소유권 계약 추가 |
| [doca/peer_wire_rdma.c](doca/peer_wire_rdma.c), `rdma_conn`, `rdma_arm_conn` | 연결별 MR, SEND 16개/RECV 32개 slot, SEND busy bit | slot state와 lease generation 추가. 기존 MR/pool을 재사용 |
| 같은 파일, `rdma_send_msg`, `rdma_recv_msg` | SEND slot으로 복사 / RECV slot에서 복사 후 즉시 repost | lease 기반 primitive 구현, 기존 API는 copy compatibility wrapper로 유지 |
| 같은 파일, `rdma_poll_cq`, `rdma_release` | CQ에서 slot 반환, QP 파괴 후 MR 해제 | POSTED/LEASED 구분, stale token/completion와 shutdown 수명 검증 |
| [doca/peer_transport.c](doca/peer_transport.c), `conn_feed` | RECV→`c->in`→`peer_tls_in` | RX lease를 TLS에 직접 전달한 뒤 반환 |
| 같은 파일, `conn_drain`, `conn_flush`, `transport_send` | BIO→`c->out`→wire, 막히면 `out_len` 유지 | TX slot에 직접 drain, BIO pending까지 포함한 backpressure |
| 같은 파일, `conn_step`, `transport_recv`, `dmesh_peer_transport_pending` | handshake/prologue/수신/progress | 모든 TLS 출력 발생 지점에 공통 output pump와 정확한 pending 조건 적용 |
| 같은 파일, `conn_alloc`, `conn_free` | `calloc(2, PEER_WIRE_MSG_MAX)`, `free(c->out)` | borrowed slot을 소유 allocation과 분리; cleanup 순서 수정 |
| [doca/peer_tls.c](doca/peer_tls.c), `peer_tls_out`, `peer_tls_in` | 각각 `BIO_read`, `BIO_write` | 초기 구현은 함수와 crypto를 그대로 유지. caller의 buffer만 교체 |
| [doca/peer_wire_tcp.c](doca/peer_wire_tcp.c), TCP ops | 기존 copy API | 초기에는 lease callback NULL, 기존 경로 유지 |

`peer_transport.c`와 `peer_wire_rdma.c`만 바꾸고 끝내지 않는다. interface 소비자인
in-memory test carrier와 close/pending 경로를 함께 바꿔야 한다. public native API나
peer frame format을 바꾸는 작업은 아니다.

### 11.20.3 A — 공통 lease API 계약

lease는 “buffer를 잠시 사용할 권리와 그 반환 token”이다. raw pointer만 반환하면
slot 재사용 후 이전 caller의 release를 구별할 수 없으므로 token이 필요하다.
다음 type과 callback을 `peer_wire.h`에 추가하는 것을 시작안으로 한다.

```c
/* 제안: connection epoch, slot index, 해당 slot의 lease generation을 검증한다. */
struct peer_wire_token {
    uint64_t conn_epoch;
    uint64_t lease_generation;
    uint32_t slot;
    uint32_t kind;                 /* TX / RX: 교차 사용 금지 */
};
struct peer_wire_tx_lease {
    uint8_t *data;
    size_t cap;
    struct peer_wire_token token;
};
struct peer_wire_rx_lease {
    const uint8_t *data;
    size_t len;
    struct peer_wire_token token;
};
/* peer_wire_ops 끝에 추가할 optional callback */
int (*tx_reserve)(void *wc, struct peer_wire_tx_lease *out);
int (*tx_commit)(void *wc, struct peer_wire_tx_lease *lease, size_t len);
int (*tx_cancel)(void *wc, struct peer_wire_tx_lease *lease);
int (*rx_acquire)(void *wc, struct peer_wire_rx_lease *out);
int (*rx_release)(void *wc, struct peer_wire_rx_lease *lease);
```

- TX 3개/RX 2개는 각각 한 묶음이다. transport 생성 시 묶음 안에 일부 callback만 있으면
  config error로 거부한다. TX만 또는 RX만 먼저 지원하는 단계는 허용한다.
- `reserve/acquire`: `1`은 lease 획득, `0`은 지금 자원/완료 없음, `-1`은 fault다.
  `0/-1`이면 out을 비우고 소유권을 주지 않는다. TX cap은 현재 `PEER_WIRE_MSG_MAX`
  전체를 수용한다. initial scope는 connection당 각 방향 outstanding lease 최대 1개다.
- `commit`: 유효한 lease의 `0 < len <= cap`을 한 번에 post하고 `1` 반환한다.
  성공한 reserve는 SEND WR 자리까지 확보하므로 **commit에는 would-block `0`이 없다**.
  post 실패는 terminal `-1`이며 TLS byte를 재암호화해서 복구하지 않는다.
- 유효 token으로 commit하면 성공/실패 모두 caller의 lease는 소비된다. invalid length면
  post 없이 connection fault 처리하고 예약을 회수한다. `cancel/release`는 성공 `0`,
  fault `-1`이며 유효 lease는 실패 때도 소비된다. 반환 후 struct를 비워 재사용을 막는다.
- invalid/stale/다른 방향 token은 `-1`로 거부하되 현재 다른 owner의 slot에는 손대지 않는다.
  pointer 산술만 믿지 말고 epoch/slot/generation/state/길이를 검사한다. generation wrap 시
  이전 token과 충돌시키지 말고 connection을 retire하는 정책으로 닫는다.
- 새 연결 generation은 현재 RDMA WR epoch 검증과 일관되게 부여하되 lifetime이 다른 lease
  generation을 별도로 둔다. 기존 WR packing의 epoch bit 수를 무심코 바꾸지 않는다.
- 모든 호출은 현재처럼 동일 worker가 직렬 실행한다. 임의 thread가 callback을 호출하는
  기능은 추가하지 않는다. lease를 타 worker/async DMA에 넘기는 확장은 별도 계약이 필요하다.
- callback 존재 여부에 따른 **copy compatibility 경로**는 허용한다. 이는 TLS/RDMA를
  plaintext/TCP로 바꾸는 security/transport fallback과 다르다. RDMA lease 모드 실행 중
  자원 부족을 숨기려고 임시 malloc+copy로 우회하지 않는다.

### 11.20.4 A — RDMA slot 상태와 기존 API 호환

기존 pool 크기와 MR 등록은 유지한다. packet마다 `ibv_reg_mr()`를 호출하거나 caller의
임의 heap buffer를 즉석 등록하지 않는다. slot state는 다음처럼 구분한다.

```text
TX: FREE → RESERVED → POSTED → FREE (성공 SEND CQ)
                   ↘ FREE          (post 전 cancel)
RX: POSTED → READY → LEASED → POSTED (release 후 repost 성공)
각 상태에서 terminal fault → 새 대여 중단 → owner 정리 → QP 파괴 → MR/pool 해제
```

1. `send_busy[]`의 역할을 FREE/RESERVED/POSTED state로 확장한다. reserve 시 FREE만
   선택하고 바로 RESERVED로 바꾼다. 아직 post하지 않은 예약을 legacy `send_msg()`나
   다음 reserve가 다시 사용하지 못하게 한다.
2. commit은 같은 slot 주소/`c->mr->lkey`를 SGE에 넣어 기존 `IBV_WR_SEND`와
   `IBV_SEND_SIGNALED`로 post한다. NIC가 읽는 동안 pointer를 caller에게 다시 주지 않는다.
   CQ는 epoch가 일치하는 POSTED slot만 FREE로 만든다. completion error면 connection fault다.
3. RX CQ는 slot을 READY로 만들고 기존 `rq_slot/rq_len` queue에 넣는다. acquire는
   가장 오래된 READY를 dequeue해서 LEASED로 바꾸지만 **repost하지 않는다**. release에서
   `rdma_post_recv()`하고 성공 때만 POSTED로 바꾼다. repost 실패는 terminal fault다.
4. queue count만 보고 free slot 수를 계산하지 않는다. LEASED slot은 queue 밖에 있어도
   소비 중이다. fault 전 정상 상태에서는 TX/RX 각 pool의 상태 개수 합이 16/32여야 한다.
5. legacy `rdma_send_msg()`는 reserve→memcpy→commit으로 구현한다. 반환 `1` 뒤 caller가
   원본을 재사용할 수 있다는 기존 계약을 유지한다. legacy `rdma_recv_msg()`는
   acquire→cap 검사→memcpy→release로 구현하고 cap 초과의 기존 terminal error를 보존한다.
6. lease와 legacy API를 섞어도 pending RX lease를 건너뛰어 다음 message를 먼저 전달하지
   않는다. 겹친 acquire/reserve는 `0`으로 거부한다. token 중복 반환은 위의 `-1` 규칙이다.
7. close 시 새 대여를 막고 caller의 lease 사용을 먼저 끝낸다. transport가 들고 있는
   미제출 TX는 cancel, RX는 release/terminal 정리 후 wire close를 호출한다. QP 파괴로
   DMA가 더는 접근하지 않는 상태를 만든 뒤 MR과 pool을 해제한다. **close 후 pointer를
   사용하거나 해제된 wc로 release를 호출하는 동작은 금지**한다.

현재 TLS adapter는 lease를 동기적인 pump 호출 안에서만 사용하므로 비동기 사용자 종료를
기다리는 별도 큐는 필요 없다. 미래 IPsec consumer가 slot에서 비동기 DMA를 시작한다면
완료/취소와 close barrier를 추가한 뒤 사용해야 하며, 이 초기 구현이 그 수명까지 해결했다고
주장하지 않는다. local SEND CQ는 slot 재사용 조건일 뿐, source custody의 `DELIVERED`는
계속 destination `REV_DONE` 뒤 `STREAM_ACK`로만 결정한다.
일반 SEND buffer 재사용 조건은 [rdma-core ibv_post_send 문서](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_post_send.3)를 따른다.

### 11.20.5 B — TLS RX 연결부터 구현

`conn_feed()`에서 RX callbacks가 있으면 아래 순서로 바꾸고, 없으면 기존 `c->in` 경로를
사용한다. 다음은 성공/실패 시 반드시 lease를 반환하기 위한 의사코드다.

```c
struct peer_wire_rx_lease rx = {0};
int r = wire->rx_acquire(c->wc, &rx);
if (r <= 0)
    return r;                        /* 0: 아직 없음, -1: fault */
int fed = peer_tls_in(c->tls, rx.data, rx.len);
int released = wire->rx_release(c->wc, &rx); /* fed 실패 때도 호출 */
return (fed == 0 && released == 0) ? 1 : -1;
```

현재 `peer_tls_in()`은 memory BIO에 `BIO_write()`하고, 전체 입력이 복사됐을 때만
성공한다. 그러므로 성공 반환 즉시 RX slot을 반환해도 된다. `SSL_read()`나 application
delivery까지 기다리지 않는다. 실패 시에는 부분 입력이 있었을 수 있으므로 같은 message를
다시 feed하지 않고 connection을 fault 처리한다. release의 repost 실패도 성공으로 숨기지 않는다.

입력 BIO로의 복사는 남는다. 이번에 없어지는 것은 `RECV slot → c->in` 한 번이다.
향후 custom BIO가 외부 pointer를 보관하도록 변경되면 이 반환 시점도 반드시 재설계한다.
[OpenSSL memory BIO 문서](https://docs.openssl.org/3.0/man3/BIO_s_mem/)와 현재
`peer_tls_in()` 구현을 함께 기준으로 삼는다.

### 11.20.6 B — TLS TX 연결과 재암호화 방지

TLS는 `SSL_write()`로 받은 평문을 이미 session sequence에 반영한다. 그 뒤 slot이 없다는
이유로 상위에 `0`을 반환하면 상위가 같은 평문을 다시 보내므로 중복 전송이 된다.
기존 `transport_send()`의 “이전 출력이 밀려 있으면 새 평문을 받지 않음”을 그대로 보존한다.

새 lease 경로는 `c->out` 대신 **출력 BIO 자체에 아직 제출하지 못한 암호문을 유지**한다.
추가 heap queue를 만들지 않는다. 공통 output-pending predicate를 추가한다.

```text
pending_output = legacy out_len != 0 || peer_tls_out_pending(tls) != 0
```

TX pump의 순서:

1. `peer_tls_out_pending()`이 0이면 아무 slot도 빌리지 않고 끝낸다. pending bytes가
   `PEER_WIRE_MSG_MAX`를 넘으면 기존 bounded-output 계약 위반으로 fault 처리한다.
2. slot reserve가 `0`이면 BIO를 읽지 않는다. bytes를 BIO에 남기고 CQ에 의한 자원 반환을
   기다린다. `-1`이면 connection fault다. 성공 시 slot cap도 검사한다.
3. 기존 `peer_tls_out()`을 호출하되 출력 주소를 `tx.data + written`으로 바꿔 채운다.
   전체 pending을 cap 내에서 drain한다. 남은 output이 cap을 넘거나 예상과 다르게 읽기가
   멈추면 무한 반복하지 않고 cancel/fault한다. empty output은 cancel하며 zero-length SEND는 없다.
4. 채운 길이로 commit한다. commit 성공 뒤 slot은 NIC owner이고 transport는 pointer를
   버린다. commit fault는 이미 BIO에서 소비한 bytes를 재생성하지 말고 connection을 닫는다.
5. 반환은 실제 bytes를 제출했으면 progressed, slot 부족이면 no-progress다. “대기 중”을
   “진행함”으로 보고 worker를 busy-loop시키지 않는다.

`transport_send()`도 함께 변경한다.

```text
이전 output pump → 여전히 pending이면 0 반환 (새 평문은 아직 TLS에 넣지 않음)
→ peer_tls_write(이번 평문) 정확히 한 번
→ 새 output pump
→ fault면 -1, 아니면 평문 len 반환 (BIO에 output이 남아도 이미 accepted임)
```

`conn_drain/conn_flush`는 이 공통 pump를 호출하는 구조로 정리하거나 하나로 합친다.
lease 경로와 legacy 경로가 동일 BIO를 각각 읽지 않게 한다. RX만 lease인 단계에서는
TX legacy 경로를 그대로 두어 incremental test가 가능하게 한다.

**DATA send만 변경하면 불완전하다.** 다음 call site를 모두 확인한다.

- `conn_step()`의 TLS handshake: 이전 output을 우선 pump하고, 막혀 있으면 새 handshake
  step으로 output을 계속 누적하지 않는다. handshake 완료와 마지막 output 제출 상태를
  따로 추적한다. RX feed/progress를 무조건 멈추지는 않아 상호 backpressure를 풀 수 있게 한다.
- initiator prologue: `out_len == 0` 대신 pending predicate를 사용하고 prologue를 TLS가
  받아들였다는 상태를 명시한다. 제출 대기 때문에 prologue를 다시 `SSL_write()`하지 않는다.
- `transport_recv()`와 responder의 `conn_read_prologue()`: `peer_tls_read()`도 TLS control
  output을 만들 수 있으므로 호출 후 output pump를 실행한다. pending output이 있는 동안
  새 SSL 처리로 계속 쌓지 않는다. 읽은 plaintext는 출력 대기만으로 버리거나 다시 읽지 않는다.
- pending일 때 inbound를 BIO에 feed할 필요가 있다면 unread input 총량에도 명시적인 bound를
  둔다. 무제한 RX drain으로 registered pool의 제한을 unbounded BIO queue로 옮기지 않는다.
  초기 제안은 unread input `PEER_WIRE_MSG_MAX` 이내이며 다음 message가 이를 넘으면 lease를
  acquire하기 전에 backpressure한다. peek/길이 확인이 없다면 full-size 1개가 들어갈 공간을
  보수적으로 확보한 뒤 acquire한다. input pending 조회 helper가 필요하면 `peer_tls.h/c`에 추가한다.
- `dmesh_peer_transport_pending()`과 ESTABLISHED progress: `out_len`뿐 아니라 BIO pending도
  포함하고, 다음 application write가 없어도 output을 재시도한다. CQ event를 읽고 slot이
  돌아오는 기존 epoll/progress 경로를 유지한다. 같은 blocked 상태만으로 progress=1을 반환하지 않는다.

이 gating은 아직 구현되지 않았다. 특히 양방향 SEND ring 포화 + TLS control output test가
통과하기 전에는 “memory BIO에 남기면 끝”으로 구현 완료 처리하지 않는다. 암호화 backend를
바꾸거나 custom TLS record 생성으로 이 문제를 우회하지 않는다.

### 11.20.7 allocation, 실패 처리와 호환 경로

현재 `conn_alloc()`은 하나의 2×73728-byte allocation을 `out/in`으로 나누며
`conn_free()`는 두 영역을 cleanse하고 `free(c->out)`한다. 여기에 borrowed SEND pointer를
그냥 대입하면 registered pool을 잘못 free/cleanse하므로 금지한다.

1. 초기에는 기존 allocation을 유지해도 된다. 소유 buffer와 일시 lease를 별도 field/local
   variable로 두고 correctness부터 닫는다. 이 단계도 두 중간 memcpy는 제거할 수 있다.
2. 이후 방향별 owned scratch allocation으로 분리한다. TX lease 지원 시 out scratch,
   RX lease 지원 시 in scratch를 생략한다. 어느 방향이 NULL이어도 cleanup이 안전해야 한다.
3. cleanse/free는 자신이 소유한 allocation에만 적용한다. borrowed slot을 직접 free하거나
   NIC가 사용하는 POSTED buffer를 cleanse하지 않는다. 기존 secret cleanup 보장은 유지한다.
4. lease callbacks 부재 시 TCP/in-memory carrier는 종전 copy 경로를 사용한다. security
   identity, frame bytes, auth timeout과 channel/custody 반환 값은 두 경로에서 같아야 한다.
5. production 기본값 전환 전에 copy 경로와 lease 경로를 각각 강제하는 test fixture를 둔다.
   이를 위해 평문 모드나 새 환경변수를 임의로 추가하지 않는다.

### 11.20.8 구현 순서와 따라 실행할 검증

작업 순서는 **A의 계약/테스트 → A의 RX + B의 RX → A의 TX + B의 TX → scratch 정리**다.
각 단계는 기존 test를 유지하는 작은 변경으로 나눈다.

1. `peer_wire.h`의 계약과 callback validation부터 추가한다. 기존 TCP와 모든 ops initializer가
   계속 build되는지 확인한다. callbacks NULL 경로는 그대로 동작해야 한다.
2. [tests/peer_transport_test.c](tests/peer_transport_test.c)의 기존 `STUB_OPS`는 copy regression으로
   보존하고, bounded leased carrier fixture를 별도로 추가한다. 현재 stub의 malloc queue를
   단순히 pointer 반환하도록 바꾸는 것으로 실제 lifetime test를 대신하지 않는다.
3. lease fixture에는 reserve 부족, delayed SEND completion, held RX, commit/repost failure와
   stale generation을 주입한다. 공통 slot 상태 처리를 hardware 없이 테스트하려면 해당
   helper를 production RDMA 코드와 공유해 mock에서만 올바른 별도 구현을 만드는 것을 피한다.
4. `rdma_conn` RX state/acquire/release 및 legacy wrapper를 구현하고 `conn_feed()`에 연결한다.
   실제 `peer_tls_in()`을 사용해 release 직후 원래 RX buffer를 poison/재사용해도 이후
   복호화 결과가 변하지 않는지 확인한다. 이 test는 BIO가 입력을 소유했다는 증거다.
5. TX state/reserve/commit/cancel과 legacy wrapper를 구현한다. 이어 output pump, send,
   handshake/prologue/read/pending을 함께 바꾼다. `test_send_backpressure()`를 copy/lease 양쪽에
   적용하고 TLS가 받은 frame 수와 destination 수신 seq가 정확히 한 번인지 검사한다.
6. [tests/peer_wire_test.c](tests/peer_wire_test.c)에 실제 RDMA lease test를 추가한다. 장비가
   없으면 기존처럼 RDMA arm은 SKIP이다. TCP roundtrip 성공을 RDMA lease 검증으로 세지 않는다.
7. [tests/peer_tls_test.c](tests/peer_tls_test.c)의 암호화/auth regression과
   [tests/peer_channel_test.c](tests/peer_channel_test.c)의 custody/lifecycle regression을 유지한다.
8. 불필요 scratch allocation을 제거한 뒤 sanitizer와 종료 후 slot/lease 잔량을 다시 검증한다.

필수 negative/lifetime test:

- TX 16 slot을 completion 없이 채운 뒤 다음 reserve=0, 기존 POSTED bytes 불변.
  CQ 반환 뒤에만 재사용. reservation cancel은 SEND를 생성하지 않음.
- RX release 전 같은 slot에 새 receive를 post하지 않음. 32-slot 소유권 합계 보존,
  release/repost로 진행 재개. FIFO, cap 초과, stale/중복/다른 방향 token 거부.
- TLS 이전 output blocked 상태에서 새 frame의 SSL write가 0회; 이미 accepted된 frame은
  재시도에도 SSL write가 1회. 해제 후 output 순서·bytes·destination delivery가 정확함.
- slot이 막힌 handshake/prologue와 양방향 traffic, idle 후 남은 BIO output, read가 생성하는
  control output, input bound에서의 backpressure. lost wake나 무한 progress 없음.
- commit 실패, BIO feed 실패, repost 실패, active lease/POSTED 상태의 close, reconnect 뒤
  old CQ/token: use-after-free, double return, lease leak 없이 기존 terminal custody 처리.
- SEND CQ만 완료하고 destination ACK를 지연하면 source `DELIVERED`는 아직 발생하지 않음.
  key mismatch/unbound node, FIN/POD_GONE와 transport loss의 기존 거부 범위 보존.

기존 [Makefile](Makefile)의 target으로 구현 후 실행할 명령은 다음과 같다. 이번 문서 작업에서
실행한 결과가 아니며, 필요한 OpenSSL/rdma-core 개발 dependency가 있는 환경에서 수행한다.

```sh
make build/test/peer_tls_test build/test/peer_transport_test build/test/peer_channel_test build/test/peer_wire_test
./build/test/peer_tls_test
./build/test/peer_transport_test
./build/test/peer_channel_test
./build/test/peer_wire_test
make test-hostfree
make test
```

현재 `test-hostfree`에는 `peer_transport_test`/`peer_wire_test`가 포함되지 않는다. 따라서
그 target만 통과했다고 lease regression 완료로 보고하지 않는다. 새 독립 state test 파일을
만들 경우 Makefile target과 CI 호출도 함께 추가한다. ASan/UBSan은 별도 build directory에서
해당 test를 다시 compile하고 실행하며 정상 build artifact와 섞지 않는다.

### 11.20.9 성능·완료 기준과 IPsec 이후 재사용

실제 감소량은 정상 처리한 **wire ciphertext bytes** 기준으로 TX/RX 각 한 번의 중간 복사다.
application payload 길이는 TLS overhead와 다르므로 혼용하지 않는다. 제안 counter는
TX/RX lease bytes, compatibility-copy bytes, reserve blocked, live/high-watermark lease,
commit/repost fault다. bounded worker aggregate로 집계하며 Pod별 metric label을 추가하지 않는다.
BIO copy와 frame assembly까지 이 counter 하나로 측정했다고 표현하지 않는다.

hardware A/B는 기존 copy, RX-only, TX-only, TX+RX를 같은 TLS/backend·compiler·ring depth·
payload·concurrency에서 비교한다. 작은 message와 1/8/64 KiB payload, 단방향/양방향을
구분하고 throughput, CPU/byte, p50/p99, memory 사용과 RNR/queue stall을 남긴다.
이번에는 `max_send_sge`, ring depth, IPsec, Linkerd batching을 동시에 바꾸지 않는다.

- [x] 공통 API가 TLS/BIO type을 노출하지 않으며 owner/state/token 계약 test가 있다.
- [ ] TLS+RDMA lease 경로에서 TX `c->out→slot`, RX `slot→c->in` copy bytes가 각각 0이다.
- [ ] TLS wire/auth 결과, frame 순서와 ACK/custody contract가 copy 경로와 동일하다.
- [ ] backpressure/종료/재연결 뒤 dangling pointer, stale release, slot leak가 없다.
- [ ] hardware RDMA 검증과 성능 A/B를 별도로 기록했다. CPU/p99/throughput 회귀가 있다면
  lease hold 시간·batching·progress 변경을 설명하고 수정하기 전에는 완료로 표시하지 않는다.

나중에 IPsec data mode를 선택하면 A의 API/pool/state machine을 재사용하고 B의 TLS
producer/consumer만 교체한다. RX plaintext를 parser나 host DMA가 참조하면 그 사용이 끝난
뒤 release해야 한다. TLS 때의 “BIO_write 직후 release”를 그대로 적용하면 안 된다.
frame 조립까지 송신 slot에 직접 하는 추가 최적화는 이 API를 활용할 수 있지만, 현재
`channel->tx_frame`의 retry/소유권 계약 변경은 별도 후속 계획이다.

---

# 12. P3: Go와 추가 통합

Go 작업은 P1 API freeze와 P0 안정성 뒤 팀원과 진행한다.

## 12.1 왜 preload로 해결할 수 없는가

preload는 dlsym(RTLD_NEXT)로 libc socket call을 interpose한다. static binary와
libc를 거치지 않고 syscall을 내는 Go runtime에는 적용되지 않는다. Go 지원을
source change 없이 된다고 약속하지 않는다.

## 12.2 권장 surface

- C native API를 감싼 작은 cgo transport
- net.Conn 구현
- net.Listener 구현
- context cancellation/deadline
- SetDeadline
- half-close 또는 지원하지 않는 semantics의 명시
- Go runtime poller와 연결할 fd/eventfd
- gRPC custom dialer와 listener

Go wrapper는 별도의 registration, identity, retry 정책을 만들지 않는다. 같은
channel과 host가 검증한 paired-DPU 등록 identity를 사용한다.

## 12.3 시작 조건

- [ ] public C API thread/error/teardown freeze
- [ ] transport loss contract 결정
- [ ] broker lifecycle PASS
- [ ] 최소 native conformance fixture
- [ ] 팀원과 ownership 및 review 범위 합의

## 12.4 완료 조건

- [ ] net.Conn behavior test
- [ ] net.Listener accept test
- [ ] cancellation/deadline/backpressure
- [ ] gRPC Go unary/streaming
- [ ] process/channel teardown
- [ ] actual DPU hardware smoke
- [ ] C++ gRPC와 같은 admission/security path 사용

---

# 13. 계속 미뤄온 항목

이 절은 잊지 않기 위한 목록이지만 P0~P3보다 먼저 하지 않는다.

## 13.1 High-concurrency quiescence gate

상태: OPEN, P0 worker progress에 포함.

grpc_closed_sweep.sh가 8KiB total concurrency 4096 이상 point 사이 session,
task, backend channel, DMA residue를 검사하지 않는다. gate를 추가한 뒤 rejected
stress sequence를 다시 실행한다.

완료:

- point마다 clean pre/post state
- residue가 있으면 다음 point 중단
- overload 자료와 capacity 자료 분리

## 13.2 ARM worker per-event fixed cost

상태: 진행 중. driver 루프의 고정비는 아래까지 줄였고, 남은 병목은 스택 task와
DOCA 내부에 있다.

현재 driver 루프는 다음과 같이 돈다. Rust drain이 C engine drain보다 먼저 실행되어
같은 pass 안에서 stack 출력이 DMA로 넘어간다. endpoint는 stack이 건드렸을 때만
dirty 비트를 세우고 worker signal 하나를 올리므로 driver는 idle endpoint를 lock 없이
건너뛴다. maintenance 마감은 timer entry 하나를 주기마다 reset해서 잡고, wait가
끝나면 실제로 울린 notification source만 clear하며, wake eventfd는 tick이 게시된
경우에만 한 번 읽는다. vendored h2 patch로 client connection poll은 요청당 약 2회다.

[park-arm-20260906](bench/report/data/park-arm-20260906/SUMMARY.md) 기준 gRPC 64 B의
요청당 worker syscall은 epoll_pwait 8.3→7.0, write 1.2→0.68, read 2.4→2.7이고,
CPU/RPC는 capacity 64 B/1 KiB/8 KiB/64 KiB에서 −2.2/−2.4/−1.3/−3.0%, matched에서
−1.7/−1.7/−2.6/−3.3%, opaque TCP에서 −9.7~−12%다. callchain으로 본 syscall의
주인은 다음과 같다.

- epoll_pwait의 절반 이상은 tokio `park_yield`(epoll 0)다. h2/hyper task가 coop
  예산(128연산)을 소진해 `defer`되면 runtime이 I/O driver를 한 번 폴링한다. driver
  task 자신의 yield는 이 폴링에 무임승차하므로 driver 쪽에서는 못 없앤다.
  나머지는 DOCA PE의 `clear_notification`과 `request_notification` 내부 epoll이다.
- read의 87%는 `mlx5dv_devx_get_event`(PE event channel)다. 울린 PE만 clear하면
  clear의 read는 줄지만 건너뛴 event를 다음 arm에서 DOCA가 읽으므로 총수는 비슷하다.
- write는 DMA 완료 콜백의 `dpu_request_host_doorbell → dpu_wake_main`과 DOCA arm의
  eventfd write가 남는다.
- arm 1.8회/RPC 가운데 실제 스레드 park는 0.4~0.7회다. select!가 Pending을 낸 직후
  stack task가 signal을 올려 깨우는 pass는 arm의 DOCA syscall만 소비한다.

남은 레버와 위험:

- h2 연결 task를 `tokio::task::unconstrained`로 spawn하면 coop `defer`가 사라진다.
  spawn 지점은 vendored linkerd의 `linkerd/proxy/http/src/h2.rs`와
  `linkerd/app/src/dmesh.rs` 두 곳이다. task 공정성을 끄는 변경이므로 입력이 drain
  예산으로, 출력이 chunk로 유계임을 근거로 두되 별도 A/B 없이는 적용하지 않는다.
- arm 전에 stack task에 한 번 더 양보하고 다시 drain하면 헛 arm이 준다.
- host doorbell을 main 스레드가 소유하는 구조는 wake write를 남긴다. 소유권 설계 항목이다.
- 요청당 runtime pass 약 21회와 Linkerd service stack 깊이는 그대로다.

목표는 100 RPS에서 200 ARM µs/RPC 이하, closed-loop single request p50 0.7 ms 미만,
knee regression 없음이다.

## 13.3 남은 session cost attribution

상태: DEFERRED.

per-workload stack sharing으로 session당 ARM time은 약 3.9ms에서 0.4~0.5ms로
줄었다. 동기 stack build 중 약 148.4µs는 계측됐고, 남은 것은 policy/destination
discovery, reconnect layer, endpoint/balancer construction 등 asynchronous
boundary다.

SessionMetrics 하나를 확장해 원인을 분해하며 별도 metric surface를 만들지 않는다.

## 13.4 Linkerd TX 경로: arena batching과 고정비

상태: 구현과 hardware A/B 완료. 전송 계약은 [design/DATA.md §4.3](design/DATA.md)과
§18.1이 정의하고, 남은 항목은 이 절 끝의 목록이다.

Linkerd 출력은 endpoint별 미게시 64 KiB arena chunk에 한 번 복사되고 driver pass가
chunk 단위로 DMA 또는 peer custody로 넘긴다. 이 경로의 비용은 네 번의 같은 rig A/B로
확정했다. 각 단계는 직전 단계 binary와 같은 날 같은 geometry에서 비교한 값이며, 단계
사이 곱은 다른 날 캠페인을 잇는 것이라 rig 편차를 포함한다.

| 단계 | receipt | gRPC capacity CPU/RPC 변화 64 B / 1 KiB / 8 KiB / 64 KiB | 무엇이 바뀌었나 |
|---|---|---:|---|
| Vec → direct | [direct-tx-rdma-20260905](bench/report/data/direct-tx-rdma-20260905/SUMMARY.md), [direct-tx-diagnosis-20260906](bench/report/data/direct-tx-diagnosis-20260906/SUMMARY.md) | −0.2 / +3.4 / +2.5 / +5.2% | 중간 Vec 제거, write마다 즉시 publication |
| direct → batch | [arena-tx-batching-perf-20260906](bench/report/data/arena-tx-batching-perf-20260906/SUMMARY.md) | −1.0 / −3.6 / −2.7 / −3.2% | endpoint별 chunk에 여러 write를 모아 pass마다 publication |
| batch → lean | [tx-fixed-overhead-20260906](bench/report/data/tx-fixed-overhead-20260906/SUMMARY.md) | −4.4 / −3.4 / −2.9 / −3.8% | writer quota·grant 삭제, endpoint lock 통합, waker/thread 캐시, dirty 비트와 worker signal |
| lean → lean3 | [park-arm-20260906](bench/report/data/park-arm-20260906/SUMMARY.md) | −2.2 / −2.4 / −1.3 / −3.0% | maintenance timer 고정, 울린 PE만 clear, wake eventfd 1회 읽기 |

측정이 확정한 사실:

- Linkerd 경로의 payload 복사는 staging→h2 buffer와 h2 buffer→arena 두 번이다. 제거된
  Vec 복사는 캐시에 있는 버퍼 사이의 복사였고, DMA가 읽고 간 cold arena로의 복사가 남았다.
  64 KiB에서 memcpy 비중은 6.41→5.60%(−0.8pp)만 줄었다. 복사 횟수가 아니라 cold 복사
  수가 비용을 정한다. 복사에서 더 얻으려면 TX가 staging 참조 piece를 publish하거나
  RX 복사를 없애야 한다.
- write당·pass당 고정비는 64 B에서는 보이지 않고 64 KiB에서 보인다. publication은
  호스트 도착 단위(약 9.5 KB)에 묶이므로 64 KiB 요청 하나가 write와 pass를 약 7회 만든다.
- 64 KiB PMU는 instructions/RPC와 cycles/RPC가 단계마다 같이 줄었다. batch까지는
  instruction이 늘고 cycle이 줄어 stall 성격이었고, lean 이후로는 instruction 자체가 준다.
- opaque TCP는 요청당 CPU가 작아 syscall 절감이 크게 드러난다(lean→lean3 −9.7~−12%).
  gRPC의 capacity RPC/s 변화는 작고 64 B/8 KiB는 3회 범위가 겹친다. 판정은 CPU/RPC 범위
  분리로 한다.
- 측정 중 host(rapids4)에서 빌드나 테스트를 돌리면 64 KiB capacity가 8% 떨어진다.
  host가 부하 생성기이므로 캠페인 동안 host 작업은 금지한다. direct 64 KiB 재측정
  3회 산포가 13%였으므로 3회 중앙값 비교는 범위 분리를 함께 본다.
- 09-05 이전부터 있던 opaque 8 KiB/64 KiB capacity drain 실패와 고동시성 heap
  corruption은 이 작업이 해결하지 않았다.

열린 결함: DPU 런타임이 로그·dmesg·journal·core 없이 사라진 사례가 두 번 있다
(batch binary는 controller feed가 새 Pod를 싣기 전 첫 dial이 거절돼 세션 8개가
poison된 직후, lean2 binary는 run 전환 직후). sudo journal에 kill 기록이 없고 main
루프는 `while (true)`라 정상 return이 불가능하다. `dpu_main.c`가 fatal signal
backtrace와 `atexit` 흔적을 stderr에 남기므로 다음 발생 때 경로가 남는다. 배포
harness는 rollout 뒤 5초를 기다리고 warmup을 최대 4회 재시도한다.

남은 일:

- [ ] TX가 staging 범위 안의 payload `Bytes`를 복사 대신 staging 참조 piece로 publish
      하도록 vendored h2 `FramedRead`가 staging에서 직접 `Bytes`를 만들고 RX custody를
      TX 완료까지 연장하는 설계.
- [ ] §13.2의 tokio coop `defer`와 헛 arm 레버 A/B.
- [ ] DPU 런타임 조용한 종료의 원인.
- [ ] opaque 8 KiB/64 KiB capacity drain 실패.
- [ ] 정리된 트리(reservation API 제거, driver yield 복원)의 HTTP/policy hardware
      correctness와 성능 재확인. 정리 뒤 재측정은 하지 않았다.

## 13.5 ARM worker 16개와 control CPU

상태: OPEN.

현재 array는 MAX_ARM_WORKERS=16을 허용하지만 main/control thread가 CPU index A에
pin된다. 허용 CPU 16개에서 A=16이면 worker 0과 충돌할 수 있다. A=12까지는
측정됐고 130K clean RPC/s에서 worker balance가 양호했다.

A=16 전에 main/control 전용 CPU를 예약하고 control progress가 worker load에
굶지 않는지 검증한다.

## 13.6 Matched-core Linkerd 비교

상태: DEFERRED.

기존 4.3×/4.6×/2.2× 최대 RPS 비교는 DPUmesh ARM worker 8개와
LINKERD2_PROXY_CORES=1 sidecar를 비교했다. absolute system result로는 유효하지만
per-configured-core 비교는 아니다.

향후:

- 양 Pod Linkerd proxy CPU limit 4
- direct TCP 10K control
- absolute와 per-core 결과 함께 보고

## 13.7 Node density

상태: DEFERRED.

- MAX_PODS wire ceiling: 127
- MAX_DPA_RINGS: EU당 16
- 현재 BlueField N: 32
- default K: 2
- benchmark throughput geometry K: 8

검증된 범위는 K=8에서 32 Pods, K=2에서 한 Service 48 Pods다. 127은 이론적
wire ceiling이지 검증 claim이 아니다.

127 근처에서는 다음이 먼저 문제가 될 수 있다.

- DPA poll당 ring control block scan
- Pod당 약 64MiB DPU staging의 high-watermark, 총 약 8GiB
- registration churn

127 초과는 pod/service id wire field 확대와 ABI bump가 필요하다. 실제 deployment
요구가 생기기 전에는 하지 않는다.

## 13.8 Incremental topology generation

상태: DEFERRED.

현재 Pod 하나의 churn도 전체 generation을 다시 sign/publish/fetch/verify/parse한다.
single-digit node rig에서는 dominant cost가 아니다.

향후 delta는:

- signed base version
- add/remove record
- strictly increasing version
- base가 없거나 실패하면 full generation fetch
- periodic full anchor
- refused delta가 held generation을 지우지 않음

fleet size와 churn rate에서 full republish가 dominant가 될 때 bytes/event와 DPU
parse time으로 정당화한다.

## 13.9 ARM/x86 equivalent study

상태: DEFERRED.

논문을 위한 비교이며 deployment blocker가 아니다. 같은 Linkerd revision,
control plane, compiler, flags, request size, concurrency로 x86 full-stack을 만들고
cycles, instructions, cache miss, migration, context switch, latency, throughput을
비교한다.

## 13.10 eBPF redirection

상태: DEFERRED.

eBPF는 static/Go binary의 투명한 syscall redirection 후보지만 resource ownership,
shared-memory transport, identity 문제를 해결하지 않는다. Go explicit API가
실용적이지 않다는 증거가 생길 때만 조사한다.

## 13.11 Cross-node secondary destination

상태: OPEN. 설계는 §11에 고정됐고 구현이 남아 있으며 P2에 포함한다.

현재 한 stream의 두 번째 remote destination은 peer-pin-conflict로 거부된다.
protocol-aware connection에서 request별 endpoint가 여러 node로 갈 수 있도록
§11.6의 destination-keyed pin map과 exact Pod-pair association을 구현한다.

## 13.12 Traffic-secret refresh

상태: OPEN. 설계는 §11에 고정됐고 구현이 남아 있으며 P2에 포함한다.

§11.14의 1 GiB/30분 TLS KeyUpdate, 5초 failure boundary, certificate rotation 시 새
association+bounded drain 계약을 구현하고 시험한다.

---

# 14. 이미 확인한 결과와 다시 하지 않을 추측

## 14.1 Broker와 host transport

[broker-design-20260828/SUMMARY.md](bench/report/data/broker-design-20260828/SUMMARY.md)
및
[doorbell-relay-20260901/SUMMARY.md](bench/report/data/doorbell-relay-20260901/SUMMARY.md)의
결과:

- memfd MAP_SHARED 전환은 반복 ABA에서 성능 중립이었다.
- shared broker는 simultaneous wake를 직렬화해 tail을 키웠다.
- per-Pod broker는 wake delay가 Pod 수에 따라 늘지 않았다.
- 최종 broker는 dispatch하지 않고 doorbell만 relay한다.
- workload drain의 10~100µs exponential backoff가 busy period의 broker wake를
  줄였다.
- broker build는 legacy in-process PE보다 host CPU와 conc32 성능이 나아졌다.

따라서 shared broker로 합치거나 broker에 reverse ring parsing을 다시 넣는 것은
새로운 명확한 증거 없이는 하지 않는다.

## 14.2 gRPC와 Linkerd

[grpc-professor-20260902/FINAL.md](bench/report/data/grpc-professor-20260902/FINAL.md)의
단일 node receipt:

| frame | DPUmesh closed-loop median | Linkerd sidecar | 비율 |
|---|---:|---:|---:|
| 64B | 106.8K RPS | 25.0K | 4.3× |
| 1KiB | 89.1K RPS | 19.3K | 4.6× |
| 8KiB | 34.3K RPS | 15.2K | 2.2× |

이 수치는 해당 build와 closed-loop 조건의 결과다. D4가 닫히기 전 운영 capacity나
모든 배포의 upper bound로 일반화하지 않는다.

확인된 원인:

- gRPC adapter의 per-write flush가 transport coalescing을 깨뜨렸고 제거됐다.
- embedded Rust staticlib는 자동으로 Linkerd의 allocator를 상속하지 않았으며
  jemalloc과 LTO가 유효했다.
- 작은 frame의 dominant cost는 Host core가 아니라 ARM worker의 fixed per-request
  work다.
- backend registry lock은 측정상 contention이 없었다.
- per-request endpoint selection은 matched-rate에서 유의미한 data-path 비용 차이가
  없었다.
- session stack sharing은 churn cost를 크게 줄였고 steady request cost는 바꾸지
  않았다.

## 14.3 Worker spin에 대한 기존 교훈

internal poll이 event가 아니라 지속 state로 Ready를 반환하면 runtime driver가
notification fd를 progress할 기회를 잃는다. 이때 worker CPU는 99.9%인데 admin
listener까지 bound-and-unanswered가 된다. FIN steady state를 outstanding work로
세지 않고, unpublishable state는 maintenance period로 bound하는 guard가 이미
있다.

D4는 이 과거 bug와 signature가 같은지부터 검사하되 같다고 가정하지 않는다.

## 14.4 Harness 규칙

- reply에 rcnt가 없으면 nodata이고 FAIL이다.
- unreadable counter는 0이 아니라 NA다.
- policy/routing은 client 결과와 DPU verdict가 모두 필요하다.
- Pod를 재생성한 뒤 measurement 전에 다시 pin한다.
- Running이 아니라 Ready를 기다린다.
- geometry는 환경 기본값이 아니라 running DPU banner에서 읽는다.
- open-loop clean capacity와 closed-loop plateau를 섞지 않는다.
- evidence capture 실패가 cleanup/deploy를 막지 않게 하되 결과에는 실패를 남긴다.

---

# 15. 시험과 receipt 규율

## 15.1 기본 gate

~~~text
make test

ctest --test-dir build/grpc-release --output-on-failure

ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build/grpc-asan-clang --output-on-failure

cd linkerd/rust
cargo test --locked
~~~

hardware correctness는 배포를 변경하므로 명시적으로 해당 campaign을 실행할 때만:

~~~text
bash bench/suite/grpc_correctness.sh hardware
~~~

## 15.2 Receipt directory

새 campaign은 최소한 다음을 보존한다.

~~~text
bench/report/data/<campaign>-<date>/
├─ SUMMARY.md
├─ commands.txt
├─ environment.txt
├─ stages.csv
├─ counters-before.txt
├─ counters-after.txt
├─ dpu-banner.txt
├─ pod-state.txt
└─ raw/
~~~

environment에는 commit, dirty state, submodule revision, N/K/A, CPU pin, clock,
DOCA/BFB/firmware, Kubernetes node와 Pod image를 포함한다.

## 15.3 판정 규칙

1. stage마다 기대한 application 결과와 DPU counter를 함께 정의한다.
2. 아무 응답이 없는 것은 expected refusal이 아니다.
3. restart, drop, pending, residue를 숨기고 throughput만 PASS할 수 없다.
4. 같은 날, 같은 topology, 같은 clock, 반복 표본으로 비교한다.
5. 변경 직후 첫 warm run은 성능 판정에서 제외한다.
6. raw data는 수정하지 않고 derivation script를 보존한다.
7. 실패한 실험도 삭제하지 않고 rejected 이유를 붙인다.
8. hardware blocker와 software failure를 구분한다.

---

# 16. 최종 실행 순서

## Phase A — 현재 안정성

- [ ] current B1/B2/B5/B7 baseline receipt
- [ ] worker snapshot과 quiescence gate
- [ ] fresh deploy reproducer
- [ ] worker root cause fix
- [ ] 6/12/24시간 soak
- [ ] current capacity re-baseline

## Phase B — Kubernetes 구조 구현과 동결

- [x] per-Pod broker와 단일 host `dpumeshd` placement 확정
- [x] Kubernetes 밖의 DPU/`dpumeshd`, Device Plugin, 명시적 workload와 Restricted
  namespace 구조 확정
- [x] `dpumeshd` package, Device Plugin과 delegated worker supervisor 구현
- [x] controller node-mTLS, host 검증 등록, workload image/resource 전환과 webhook 제거
- [ ] target broker lifecycle B1~B7
- [ ] current node-agent/RBAC/hostPath/transient-unit 제거
- [ ] process restart/channel fatal contract 확정
- [ ] control/data lifecycle diagram과 코드 일치
- [ ] public C API freeze
- [ ] IPC/L7/peer internal contract freeze
- [ ] workload identity와 certificate granularity 확정

## Phase C — cross-node

- [ ] TCP workload mTLS와 fault injection
- [ ] node B와 RDMA fabric
- [ ] two-node control/identity
- [ ] remote stream과 custody
- [ ] lifecycle/policy/rotation
- [ ] performance receipt

## Phase D — integrations

- [ ] Go native transport
- [ ] net.Conn/net.Listener
- [ ] Go gRPC
- [ ] 다른 언어는 같은 conformance contract로 추가

## Phase E — 최적화와 규모

- [ ] ARM per-event cost
- [ ] session cost attribution
- [ ] Linkerd TX 중간 복사 제거 — direct와 arena batching 구현·배포·A/B 완료. 8 KiB gRPC capacity -2.17%로 성능 acceptance OPEN (§13.4)
- [x] RDMA 공통 TX/RX buffer lease — API·상태 시험·배포·BlueField CPU 비교 완료; 실제 fabric gate 별도 (§11.20.3~§11.20.4)
- [ ] TLS BIO↔RDMA direct 연결 — RX부터 적용, TX pending/재시도 보존 (§11.20.5~§11.20.9)
- [ ] A=16 control CPU
- [ ] matched-core sidecar comparison
- [ ] density beyond measured range
- [ ] incremental topology
- [ ] ARM/x86 study

Phase를 건너뛰려면 선행 phase가 필요 없다는 증거와 새 acceptance gate를 이 문서에
먼저 기록한다. 코드부터 바꾸고 이유를 나중에 맞추지 않는다.

---

# 17. 프로젝트 완료의 의미

연구 prototype 전체를 완료라고 부르기 위한 최소 조건:

- workload Pod는 PSS Restricted 아래 `hostPath`, privilege와 device 없이 admission부터
  teardown까지 동작한다.
- per-Pod broker failure는 다른 Pod에 전파되지 않고, kubelet/controller restart는
  existing data channel을 유지하며, `dpumeshd`/DPU restart는 node 범위에서 bounded
  fail-stop한다.
- worker progress가 fresh/aged deployment에서 재현 가능하다.
- public API와 내부 ABI가 문서와 contract test로 고정된다.
- same-node는 DMA isolation, cross-node는 workload mTLS라는 경계가 실제 packet과
  fault test로 증명된다.
- 두 실제 DPU node에서 data, policy, churn, recovery, rotation이 동작한다.
- native, preload, C++ gRPC가 같은 registration/security path를 사용한다.
- Go integration도 그 path를 재사용한다.
- 성능 수치는 topology, instrument, 반복과 failure 조건을 함께 공개한다.

그 전까지 가장 정확한 표현은 다음이다.

> DPUmesh는 single-node DPU service-mesh data path와 control plane이 구현·실증된
> research prototype이다. per-Pod broker의 완전한 failure receipt, stable worker
> progress, workload-identity cross-node mTLS와 real two-node RDMA receipt는 진행
> 중이다.

---

# 18. Working tree 전송 계약

이 절은 working tree의 data-path 구현과 검증 상태를 정의한다. 세부 ownership과
state machine은 [design/DATA.md](design/DATA.md)를 따른다.

## 18.1 Linkerd arena TX batching

각 `DmeshIo` endpoint는 최대 하나의 미게시 batch를 소유한다. batch는 C data plane이
빌려준 64 KiB 등록 arena chunk이며 process-unique token으로 이름 붙는다. scalar와
vectored write는 caller slice의 ordered prefix를 그 chunk에 한 번 복사하며, 소유
worker thread에서 endpoint lock을 쥔 채 `dmesh_l7_tx_batch_write`를 부른다. Rust는
token, 길이, exact route와 sealed 상태만 보관하고 caller pointer나 arena pointer는
보관하지 않는다. 첫 accepted write가 route를 고정하고, 같은 request connection을
쓰는 origin, local backend, remote backend endpoint도 각자 batch를 가진다.

batch가 찼거나 chunk가 없으면 write는 아무것도 받지 않고 writer를 park한다.
accepted write는 endpoint를 dirty로 표시하고 worker signal을 올린다. `poll_flush`는
latch된 오류를 보고할 뿐 즉시 반환한다. `poll_shutdown`은 admission을 닫고 driver가
buffered DATA를 게시한 뒤 FIN을 게시한다.

driver pass는 C engine을 drain하고, dirty이거나 재시도를 빚진 endpoint마다
`dmesh_l7_tx_batch_flush`로 batch를 게시하고, buffered batch 없이 park된 writer를
깨우고, 소비된 staging custody를 돌려주고, write half를 닫고 buffered batch가 없는
endpoint의 FIN을 순서대로 게시한다. 거절된 publication은 batch와 bytes를 유지하고
다시 복사하지 않는다. pass는 session 64개, 게시 256 KiB까지 방문한 뒤 stack에 양보한다.
일이 없는 pass는 completion과 DMA notification을 arm하고 한 번 더 drain한 뒤 그
descriptor, wake eventfd, worker signal, timer entry 하나로 잡은 1 ms maintenance
마감을 기다린다. wait 뒤에는 울린 source만 clear한다. abort와 connection close는
미게시 batch만 반환하고, 게시된 chunk는 DMA unit 또는 peer stream이 완료 경로에서
돌려준다.

| Metric | 의미 |
|---|---|
| `tx_arena_copy_bytes` | arena batch에 복사된 bytes |
| `tx_accepted_bytes` | DMA 또는 peer custody로 이전된 bytes |
| `tx_publications` | custody가 이전된 batch 수 |
| `tx_retries` | 가득 찬 batch·빈 arena로 거절된 write와 거절된 publication |
| `tx_errors` | terminal TX error |

## 18.2 RDMA carrier lease

`peer_wire_ops`는 whole-message API와 complete optional lease group을 제공한다.
TX group은 reserve/commit/cancel, RX group은 acquire/release로 구성되며 일부 callback만
등록한 carrier는 거부된다. RDMA connection은 등록 buffer의 16개 SEND slot과 32개
RECV slot을 소유한다.

```text
TX: FREE → RESERVED → POSTED → FREE
RX: POSTED → READY → LEASED → POSTED
```

각 방향에는 outstanding lease가 하나만 존재한다. token은 process-unique connection
epoch, slot, per-slot generation과 방향을 검증하며 pointer와 capacity 또는 length도
일치해야 한다. invalid token은 slot 상태를 바꾸지 않는다. SEND completion은 local
slot 재사용만 허용한다. application delivery custody는 destination `REV_DONE` 뒤의
`STREAM_ACK`에서 반환된다.

## 18.3 검증 상태

- `make test` 전체 통과
- Linkerd adapter Rust 41/41 통과
- `dmesh-doca` 30/30 통과
- gRPC release 및 ASAN+UBSAN 각각 4/4 통과
- C proxy-lane ASAN+UBSAN 통과
- 93개 hardware 표본에서 application failure, drop, overflow, worker failure와
  reorder 0
- 모든 hardware arm 종료에서 arena 1,024/1,024 free, live chunk 0

64 KiB gRPC capacity는 12,905 RPC/s, CPU 599.50 us/RPC, p99 7,610 us다.
8 KiB capacity는 21,949 RPC/s이며 performance acceptance는 OPEN이다. 측정 방법,
반복 범위, publication 분포와 PMU 결과는
[arena TX batching hardware receipt](bench/report/data/arena-tx-batching-perf-20260906/SUMMARY.md)에
보존한다.
