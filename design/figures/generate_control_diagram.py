#!/usr/bin/env python3
"""Generate the implemented DPUmesh control-plane diagram."""

from diagram_kit import (
    BLUE,
    BLUE_BG,
    GRAY,
    GRAY_BG,
    GREEN,
    GREEN_BG,
    ORANGE,
    ORANGE_BG,
    PURPLE,
    PURPLE_BG,
    Style,
    line,
    save,
    setup_figure,
    styled,
)

box, _container, arrow, terms = styled(
    Style(body_step=0.31, label_size=7.4, meaning_dx=1.75)
)


def generate_control_plane():
    fig, ax = setup_figure(20.0, 13.5, (0, 20.0), (-2.3, 12.0))
    ax.text(0.2, 11.62, "DPUmesh control plane — implemented authority and runtime placement",
            fontsize=17, ha="left")
    ax.text(0.2, 11.20,
            "Kubernetes states identity; the host proves process ownership; the DPU verifies and enforces.",
            fontsize=9.4, color="#555555", ha="left")

    for xsep in (6.5, 13.3):
        ax.plot([xsep, xsep], [0.2, 10.75], color="#ddddda", lw=1,
                ls=(0, (2, 3)))
    ax.text(3.25, 10.72, "KUBERNETES", fontsize=11, color="#555555", ha="center")
    ax.text(9.9, 10.72, "HOST LINUX / SYSTEMD", fontsize=11, color="#555555", ha="center")
    ax.text(16.6, 10.72, "BLUEFIELD ARM OS", fontsize=11, color="#555555", ha="center")

    box(ax, 0.4, 8.35, 2.5, 1.45, "Kubernetes API",
        ("Pods, Services,", "EndpointSlices", "get/list only"),
        edge=GRAY, face=GRAY_BG, body_size=7.6)
    box(ax, 3.45, 8.35, 2.55, 1.45, "Controller Pod",
        ("uid/gid 65532", "topology + feeds", "WorkloadGrant v3"),
        edge=PURPLE, face=PURPLE_BG, body_size=7.6)
    box(ax, 0.4, 4.75, 2.5, 1.45, "kubelet",
        ("Device Plugin API", "slot allocation", "one socket mount"),
        edge=BLUE, face=BLUE_BG, body_size=7.6)
    box(ax, 3.45, 4.75, 2.55, 1.45, "Workload Pod",
        ("no K8s token", "no device / hostPath", "dpumesh.io/channel=1"),
        edge=BLUE, face=BLUE_BG, body_size=7.45)

    box(ax, 6.9, 7.85, 5.9, 2.15, "dpumeshd.service",
        ("Device Plugin + slot generation", "SO_PEERCRED + cgroup v2 + starttime",
         "node mTLS + signed-feed delivery", "worker cgroups + broker supervision"),
        edge=PURPLE, face=PURPLE_BG, body_size=7.65)
    box(ax, 6.9, 4.4, 5.9, 2.15, "Per-slot broker child",
        ("directly supervised outside Kubernetes", "private PID/mount/network/cgroup namespaces",
         "uid/gid 65532; caps=0; no_new_privs", "DOCA + Comch; sealed mmap descriptors"),
        edge=ORANGE, face=ORANGE_BG, body_size=7.45)
    box(ax, 6.9, 1.25, 5.9, 1.65, "Worker accounting",
        ("dpumeshd.service/workers/pod<uid>.g<gen>",
         "cpu.max + memory.high/max + pids.max", "covered by kubelet system reservation"),
        edge=GRAY, face=GRAY_BG, body_size=7.25)

    box(ax, 13.7, 8.35, 5.85, 1.45, "Feed receiver service",
        ("unprivileged bounded installer", "digest check + fsync + atomic rename",
         "serves DPU public key"),
        edge=GREEN, face=GREEN_BG, body_size=7.55)
    box(ax, 13.7, 4.75, 5.85, 2.15, "dpumesh_dpu",
        ("nonce-bound grant verification", "topology / membership / target verification",
         "slot + incarnation + generation fences", "routing, policy and peer-channel enforcement"),
        edge=ORANGE, face=ORANGE_BG, body_size=7.55)
    box(ax, 15.25, 1.25, 4.3, 1.65, "Remote DPU",
        ("TLS 1.3 peer carrier", "node key bound by topology", "bounded stream custody"),
        edge=GREEN, face=GREEN_BG, body_size=7.35)

    arrow(ax, (2.9, 9.08), (3.45, 9.08), label="objects", color=GRAY,
          label_dy=0.30)
    arrow(ax, (6.0, 9.30), (6.9, 9.30), label="TLS 1.3 node API",
          color=PURPLE, label_dy=0.31)
    arrow(ax, (6.9, 8.82), (6.0, 8.82), label="signed result / refusal",
          color=PURPLE, label_dy=-0.30)
    arrow(ax, (1.65, 6.20), (1.65, 8.35), label="plugin lifecycle",
          color=BLUE, label_dx=0.72)
    line(ax, [(6.9, 8.05), (6.35, 8.05), (6.35, 6.75), (1.65, 6.75)],
         color=BLUE)
    arrow(ax, (1.65, 6.75), (1.65, 6.20),
          label="Register / ListAndWatch / Allocate", color=BLUE,
          label_dx=2.3, label_dy=0.20)
    arrow(ax, (6.9, 5.47), (6.0, 5.47), label="allocated slot socket",
          color=BLUE, label_dy=0.30)
    arrow(ax, (4.72, 6.20), (8.10, 7.85), label="HELLO; kernel evidence",
          color=BLUE, label_dx=0.40, label_dy=0.22)
    arrow(ax, (9.85, 7.85), (9.85, 6.55), label="launch + connected fd",
          color=PURPLE, label_dx=0.75)
    arrow(ax, (9.85, 4.40), (9.85, 2.90), label="enter bounded cgroup",
          color=GRAY, label_dx=0.75)

    arrow(ax, (12.8, 9.62), (13.7, 9.62),
          label="topology / membership / targets", color=GREEN, label_dy=0.30)
    arrow(ax, (13.7, 8.55), (12.8, 8.55), label="DPU public key",
          color=GREEN, label_dy=-0.30)
    arrow(ax, (16.62, 8.35), (16.62, 6.90), label="atomic feed files",
          color=GREEN, label_dx=0.72)
    arrow(ax, (12.8, 6.25), (13.7, 6.25),
          label="Comch", color=ORANGE,
          label_dy=0.31)
    arrow(ax, (13.7, 5.02), (12.8, 5.02), label="READY / doorbell",
          color=ORANGE, label_dy=-0.30)

    line(ax, [(15.1, 6.90), (15.1, 7.35), (12.2, 7.35)],
         color=PURPLE, dashed=True)
    arrow(ax, (12.2, 7.35), (12.2, 7.85), color=PURPLE, dashed=True)
    ax.text(13.65, 7.53, "scope HTTP → protocol-blind tunnel → node mTLS",
            fontsize=7.4, color="#626262", ha="center",
            bbox=dict(fc="white", ec="none", pad=0.7))
    arrow(ax, (16.65, 4.75), (16.65, 2.90), label="authenticated stream frames",
          color=GREEN, label_dx=-1.45, label_dy=0)
    arrow(ax, (18.05, 2.90), (18.05, 4.75), label="ACK after destination custody",
          color=GREEN, label_dx=1.30, label_dy=0)

    terms(ax, 0.4, -2.10, 19.15, 1.75, [
        ("grant", "controller-signed Pod/container/slot/nonce binding"),
        ("slot", "one Device Plugin resource and one Unix socket"),
        ("broker", "one isolated host child owning DOCA objects"),
        ("feed", "complete signed document installed atomically"),
        ("node mTLS", "URI SAN spiffe://dpumesh.io/node/<name>"),
        ("fail-closed", "invalid or unavailable authority admits no new slot"),
    ], columns=2)

    save(fig, "control_plane")


if __name__ == "__main__":
    generate_control_plane()
