> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Concurrent Comch Test Across 200 Host SFs

2026-09-09. BlueField-3 B3220, firmware 32.46.1006, DOCA SDK 3.1.0105.

**The test established one independent Comch connection on each of 200 host SFs,
kept all 200 connections active for 30 seconds, and exchanged messages in both
directions. No send, receive, or message validation errors were observed.**

| Item | Result |
|---|---|
| Host functions | PF0 `0000:94:00.0`, SF numbers 800–999, controller 1 |
| DPU | PF0 `0000:03:00.0`, one server on each host SF's representor |
| Process configuration | One host test process containing 200 clients, each using a separate SF; one DPU test process containing 200 servers |
| Concurrent connections | 200; the minimum connection count observed by the client after all connections became active was also 200 |
| Concurrent connection duration | 30.009 seconds |
| Messages | 64 bytes, validated by SF number and sequence number; each round waited for responses from all SFs, with at least 50 ms between rounds |
| Client messages sent / received | 119,800 / 119,800 |
| Server messages received / sent | 119,800 / 119,800 |
| Round trips per SF | 599 on every SF |
| Errors | 0 |
| Comch teardown | All 200 contexts on each side reached IDLE, object destruction succeeded, and both processes exited with status 0 |

The test created and activated 200 SFs from the existing reservation of 236 host
SFs without changing the firmware resource allocation settings. It used
representors and service names separate from the running DPUmesh server. It did
not enable `trust on` for the test SFs.

The host and DPU test programs were launched through sudo as root. This run did
not validate a least-privilege Pod configuration, restriction of application
device access to one SF, or restriction of that access to Comch operations.

An initial test verified one SF with 100 round trips over five seconds. The test
then expanded to 200 SFs. The final run opened a distinct DOCA device for each
SF. Host SF selection used the auxiliary device index to check `sfnum` in sysfs.
DPU representor selection checked host index 1, PF index 0, and the SF number.

The server log reports `min_live_after_all=0` because the server processed
connection teardown notifications after the client completed the test and
shut down normally. Evidence for maintaining all connections during the test
comes from the client's `min_live_after_all=200` and the completion of every
message round trip.

This test demonstrates **concurrent basic Comch connections and message exchange
across 200 dedicated SFs**. It does not validate deployment of 200 Kubernetes
Pods, the DPUmesh DMA/ring/DPA data path, the Comch producer/consumer fast path,
maximum throughput, availability of other SFs during a connection flood, device
access isolation, policy bypass prevention, or safe DMA cleanup after an
application crash. Each side used one process and one progress engine (PE), so
this test does not measure the cost of running a separate process per Pod.

The test program is [probe.c](probe.c). Raw output is available in the
[client log](200-client.log) and [server log](200-server.log).
[ports.json](ports.json) records the functions created during the experiment;
it is a historical record, not the current allocation state.
Final cleanup verification is recorded in [cleanup.txt](cleanup.txt).

During cleanup, host auxiliary devices continued to disappear asynchronously
after all SF ports had disappeared from the DPU. The initial 60-second wait for
the cleanup command timed out, while remote cleanup continued. Cleanup was
considered complete only after both the DPU ports and host devices were gone.
This observation concerns normal device teardown; it does not establish safe
resource reclamation under a malicious application or with DMA still in flight.
The existing DPUmesh server retained the same process ID throughout the test
and final verification.

The program was built against the installed SDK with the following command:

~~~sh
cc -O2 -Wall -Wextra -Wno-deprecated-declarations probe.c -o probe \
  $(pkg-config --cflags --libs doca-comch doca-common)
~~~

With the SFs ready, `probe server 200 40` was started on the DPU. After all 200
servers had started, `probe client 200 30` was started on the host. The server
exited after receiving the client's normal disconnection notifications.
Function creation and deletion were limited to the SFs used by this experiment.
