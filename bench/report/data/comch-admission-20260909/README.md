> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Comch Reconnection Cost with Immediate Server Disconnect

2026-09-09. BlueField-3 B3220, firmware 32.46.1006, DOCA SDK 3.1.0105.

**Immediately disconnecting every connection did not eliminate Arm CPU work:
the DPU test server consumed more CPU during repeated connections. This test
does not establish denial of service or interference with another SF.**

## Method

One host SF (controller 1, PF0, SF800) was temporarily created from the existing
reservation. The host opened that SF through DOCA; the DPU opened its matching
representor on PF0 `0000:03:00.0`. The test service was separate from DPUmesh.
One DPU server and one PE ran for 55 seconds. Its connection callback called
`doca_comch_server_disconnect()` on every successful connection. No application
messages, memory registrations, DMA, or producer/consumer channels were used.

Both test programs were launched through sudo as root. This was not a Kubernetes
Pod test and did not establish the minimum capabilities or device permissions
needed by an untrusted application. Selecting one SF is not evidence that the
process was restricted to that SF or to Comch operations only.

After eight seconds of idle measurement, a host process restarted eight reusable
client contexts for 12 seconds with a software target of 20 starts/second.
After an idle interval, another 12-second run targeted 200 starts/second.
Each side slept for 1 ms between loop iterations. The target is a load-generator
setting, not a firmware limit; the second run achieved about 98 connections/second.
This experiment did not identify the cause of that achieved rate or measure a
maximum connection rate. Context initialization, start/stop processing, and
polling affect the generator's throughput.

CPU time was measured with `CLOCK_PROCESS_CPUTIME_ID`. Process CPU percentage
uses 100% to mean one fully occupied core. A separate timer around each
`doca_pe_progress()` invocation includes SDK processing, callbacks, and the
disconnect calls made by those callbacks. It does not measure firmware time,
whole-system CPU usage, or interrupt work outside the process. Timing overhead
and idle polling are included; neither process was pinned to a CPU.

## Results

The following averages use complete interior one-second sampling intervals,
excluding transitions into and out of each connection phase.

| Phase | Observed connections/s | DPU server CPU (% of one core) | PE CPU time (ms/s) |
|---|---:|---:|---:|
| Idle | 0 | 0.98 | 2.26 |
| Target 20/s | 19.81 | 1.21 | 4.82 |
| Target 200/s | 97.86 | 2.08 | 13.54 |

[summary.json](summary.json) records the selected intervals and weighted means.
Raw observations are in [server.log](server.log), [client-20.log](client-20.log),
and [client-200.log](client-200.log). These are descriptive results from one
completed run, not confidence intervals or worst-case bounds. The observation
supports additional Arm work during connection churn despite immediate
disconnect; it does not support extrapolating linearly to CPU exhaustion.

Across both phases, the server received 1,412 successful connection events and
all 1,412 disconnect calls returned success. There were no recorded errors or
disconnect retries. Client start counts were 237 and 1,175. In the faster phase,
some asynchronous client state changes were still pending when its pre-cleanup
RESULT line was printed. All eight client contexts subsequently reached IDLE
in each phase; the server also reached IDLE and all three processes exited 0.

The server's `disconnect_events=0` counts its disconnect callbacks, not successful
disconnect API calls. Server-initiated disconnects did not produce that callback
in this run. `disconnect_accepted` counts successful API returns;
`pending_disconnects` counts only the test application's pending disconnect
requests. Neither counter establishes when hardware resources were reclaimed
or how many hardware connections remained active.

## SDK and firmware configuration review

The installed `doca_comch.h` exposes a maximum-client capability query, receive
queue size configuration, and disconnect. It does not expose a setter for
per-SF connection establishment frequency or an application admission callback
that rejects requests before Arm-side connection processing. The
[DOCA 3.1 Comch guide](https://networking-docs.nvidia.com/doca/archive/3-1-0-core-update/doca-comch)
states that processing new connections requires `doca_pe_progress()` and that
the handler receives a connection object. A receive queue size is not a documented
connection-request rate limit; a maximum-client query is not a configurable quota.

The reviewed [mlxconfig output](host-mlxconfig.txt), including Current and Next
Boot values for both host PFs, did not identify a Comch connection-request rate
or quota setting. This is a statement about the exposed configuration reviewed,
not proof that firmware lacks any internal protection or vendor-specific control.
No SDK upgrade or firmware configuration change was performed.

Two other documented controls must not be conflated with this requirement:

- [SF QoS `tx_max` and `tx_share`](https://docs.nvidia.com/networking/display/bluefielddpuosv392/qos%2Bconfiguration)
  specify transmit bandwidth. The reviewed documentation does not establish
  a bound on Comch connection creation work before Arm processing.
- [DOCA Management 3.3 ICM quota](https://networking-docs.nvidia.com/doca/archive/3-3-0/doca-management)
  controls memory allocation for a device or representor, subject to a hardware
  capability check. The installed SDK 3.1 environment lacks the management
  headers. Even if available, a memory allocation limit alone does not specify
  a creation/deletion rate limit or bounded cost of rejected requests.

Thus, **no applicable public per-SF pre-processing connection-rate control was
identified for the installed SDK/firmware combination**. This is narrower than
claiming that broker-free isolation is impossible. A DPU-only design would still
need enforceable processing budgets or function quarantine, bounded shared-device
cost, and memory isolation. Quarantine also needs a trusted recovery policy so an
application cannot reset its budget by reconnecting or reactivating its SF.
Availability of a normal SF under an attacking SF, unsuccessful connection
attempts, and server/SF shutdown under sustained load remain untested.

## Reproduction and cleanup

Build [admission.c](admission.c) on both systems:

~~~sh
cc -O2 -Wall -Wextra -Wno-deprecated-declarations admission.c -o admission \
  $(pkg-config --cflags --libs doca-comch doca-common)
~~~

With the dedicated test SF ready, run `admission server 55 1` on the DPU,
wait for READY and the eight-second baseline, then run `admission client 12 20`
on the host. Wait five seconds after client exit and run `admission client 12 200`.
Device access permissions are required. The source selects SF800 explicitly;
the function must be reserved for the experiment before running it.

The single test SF was deactivated and deleted after completion.
[cleanup.txt](cleanup.txt) confirms no matching host auxiliary device, DPU port,
or test process remained. The existing DPUmesh server retained PID 600626.
