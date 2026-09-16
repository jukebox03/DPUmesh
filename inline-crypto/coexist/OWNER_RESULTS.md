# Crypto owner and automatic Pod-pair inline IPsec: hardware validation

Two BlueField DPUs, rapids4 on DOCA 3.1 and jet1 on DOCA 3.5. Each run opens the
PF and the SF representor in switch mode and sends the root miss to the kernel,
which leaves OVS and TC in place. The only resources the trial claims are the
`10.77.0.1/2` SF fabric aliases and their permanent neighbors, both released at
the end. In every run the OVS and interface configuration is identical before and
after, and the IPv6 fabric ping loses no packets.

## Tools

- `dpumesh_crypto_owner`: the operational process holding the NIC's DOCA Flow
  IPsec state. It takes requests over a UNIX socket.
- `peer_crypto_owner_probe`: sends synthetic association requests over that
  socket to drive dynamic SA and rule install, rekey and removal. It creates no
  QP and no traffic.
- `peer_e2e_probe`: brings up a real verbs pair transport, `peer_manager` and the
  owner adapter on each node. The two nodes negotiate automatically over control
  TCP and exchange RDMA on the lanes the owner installed. Only the registration,
  placement and policy tables are fixtures.
- `owner_trial.py` and `e2e_run.py`: reuse the snapshot, ping and deadline
  discipline of `supervisor.py` to run those tools and judge configuration and
  connectivity preservation.

## Owner data path

A single owner session drives several scenarios in sequence, and SA and rule
install, rekey, block and removal all succeed with request status 0. The
sequences covered are directional SA install with an epoch-2 rekey, retire,
BLOCK and REMOVE (`r1,t1,r2,t2,o1,b,a`); receive-only with no transmit
(`r1,r2,b,a`); and a repeated association on the same IP pair (`r1,t1,a` twice).
Both DPUs pass graph creation, configuration preservation and clean exit
(`owner_rc: 0`).

## Automatic negotiation to encrypted RDMA end to end

`peer_e2e_probe` runs on rapids4 as client and jet1 as server. The two nodes
establish control TCP over the SF fabric IPv4 used by RoCEv2 and exchange
PROPOSE, ACCEPT and LANE_PARAMS automatically. The owner installs the
directional SAs with request status 0, the manager reports lane ready, and 256 B
and 64 KiB messages alternate in both directions over that lane
(`E2E_PASS sent=8 recv=8`). Shutdown runs Pod unregister, BLOCK and channel close
(`E2E_DONE channel_closed=1`).

The owner graph drops plaintext RoCE at `rx_plain` and unmatched QPNs at the
gate, so a successful round trip means the whole path ran: transmit SA
encryption, ESP, receive SA decryption and the QPN gate. Plaintext would be
dropped at the receiver and the round trip would fail.

## Round-trip performance: encrypted against plaintext

The manager, pair transport and path are identical; only the owner (encrypted)
and a stub adapter (plaintext, no owner) differ. Latency is a fixed-size
closed-loop ping-pong over several thousand round trips reported as sorted p50
and p99; throughput is streaming goodput.

| Message | RTT p50 (us) encrypted / plaintext | Throughput (MB/s) encrypted / plaintext |
|---:|---|---|
| 64 B | 36 / 31 | 8.7 / 9.8 |
| 8 KiB | 43 / 37 | 673 / 744 |
| 64 KiB | 81 / 74 | 1360 / 1440 |

Inline IPsec adds roughly 6 to 7 us per round trip. The cost is a fixed
per-crossing encrypt and decrypt charge that barely varies with size, and one
round trip crosses encrypt plus decrypt in each direction. Throughput overhead
is 5 to 13% and shrinks as messages grow. The latency figures are dependable;
throughput is path goodput measured while the send loop also advances the
manager, so it is an indicator rather than a pure wire ceiling.

## Scope

This validation covers the owner data path and the manager's automatic
negotiation, encrypted transport and revocation. Simultaneous isolation of two
Pod pairs, rejection of replay and plaintext injection, lossless live rekey, and
the host data path through real Kubernetes Pod registration remain.
