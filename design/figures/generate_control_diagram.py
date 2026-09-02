#!/usr/bin/env python3
"""Generate the DPUmesh control-plane diagram in PNG and PDF form."""

from diagram_kit import (  # noqa: F401  (palette names read by the figures)
    BLACK, BLUE, BLUE_BG, GRAY, GRAY_BG, GREEN, GREEN_BG, ORANGE, ORANGE_BG,
    PURPLE, PURPLE_BG, RED, RED_BG,
    Style,
    line,
    save,
    setup_figure,
    styled,
)

box, container, arrow, terms = styled()


def generate_control_plane():
    fig, ax = setup_figure(20.0, 14.0, (0, 20.0), (-2.55, 12.0))
    ax.text(
        0.1,
        11.62,
        "DPUmesh control plane — authority follows the proxy across PCIe",
        fontsize=17,
        ha="left",
    )
    ax.text(
        0.1,
        11.18,
        "The Pod's broker relays a signed identity; cluster placement is independently signed; the DPU enforces both.",
        fontsize=9.4,
        color="#555555",
        ha="left",
    )

    for xsep in (5.55, 12.55):
        ax.plot([xsep, xsep], [0.35, 10.82], color="#ddddda", lw=1, ls=(0, (2, 3)))
    ax.text(2.75, 10.72, "HOST NODE A", fontsize=11, color="#555555", ha="center")
    ax.text(9.05, 10.72, "BLUEFIELD DPU A", fontsize=11, color="#555555", ha="center")
    ax.text(
        16.20,
        10.72,
        "CLUSTER CONTROL / REMOTE PEER",
        fontsize=11,
        color="#555555",
        ha="center",
    )

    # Host: one trusted node agent owns every host-side control role, and one
    # broker per Pod owns that Pod's DOCA device and Comch connection. The
    # workload holds sealed mappings only.
    box(
        ax,
        0.35,
        8.85,
        2.20,
        1.35,
        "Workload Pod",
        ("app + client", "unprivileged,", "no device"),
        edge=BLUE,
        face=BLUE_BG,
        title_size=9.2,
        body_size=7.2,
    )
    box(
        ax,
        3.00,
        8.85,
        2.20,
        1.35,
        "Per-Pod broker",
        ("owns device", "+ Comch;", "Pod-charged"),
        edge=PURPLE,
        face=PURPLE_BG,
        title_size=9.2,
        body_size=7.2,
    )
    box(
        ax,
        0.35,
        5.45,
        4.85,
        2.55,
        "Node agent DaemonSet",
        (
            "SO_PEERCRED + cgroup -> Pod",
            "Ed25519 assertion + HMAC membership",
            "derives target feed from topology",
            "delivers identity bundle and all feeds",
            "byte-relays Linkerd/controller traffic",
            "root-owned; node-scoped Kubernetes read",
        ),
        edge=PURPLE,
        face=PURPLE_BG,
        body_size=7.65,
    )
    box(
        ax,
        0.35,
        3.40,
        4.85,
        1.25,
        "Backend Pod",
        ("live local registration", "DMA lands in application memory; no proxy hop"),
        edge=RED,
        face=RED_BG,
    )

    # DPU: local assertion and cluster topology are separate gates. Linkerd's
    # outbound stack is source-side; destination-side admission is a shared
    # inbound policy verdict rather than a second full proxy stack.
    box(
        ax,
        5.90,
        8.75,
        6.25,
        1.55,
        "Registration gate",
        ("nonce / time / replay / agent signature", "exact node + Pod + Service binding"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    box(
        ax,
        5.90,
        6.45,
        6.25,
        1.55,
        "Adopted authority",
        ("controller-signed topology generation", "node membership + derived Service targets"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    box(
        ax,
        5.90,
        3.85,
        6.25,
        1.85,
        "Embedded Linkerd + enforcement",
        (
            "source: outbound discovery / route / retry / LB",
            "destination: Pod/port inbound policy verdict",
            "identity inputs come only from signed bindings",
        ),
        edge=GREEN,
        face=GREEN_BG,
        body_size=7.75,
    )
    box(
        ax,
        5.90,
        1.15,
        6.25,
        1.55,
        "DPU-held credentials",
        ("Ed25519 static node key never leaves the DPU", "Linkerd P-256 key + certificate watch"),
        edge=ORANGE,
        face=ORANGE_BG,
    )

    # Cluster services and the remote endpoint of the designed peer channel.
    box(
        ax,
        12.90,
        8.75,
        6.70,
        1.55,
        "DPUmesh controller",
        ("Kubernetes -> Ed25519 topology generation", "mediated workload scope; DPU holds public keys only"),
        edge=PURPLE,
        face=PURPLE_BG,
    )
    box(
        ax,
        12.90,
        6.45,
        6.70,
        1.55,
        "Kubernetes API",
        ("Pods / Services / EndpointSlices / Nodes", "node-local facts + ServiceAccount token / trust roots"),
        edge=GRAY,
        face=GRAY_BG,
    )
    box(
        ax,
        12.90,
        3.85,
        6.70,
        1.85,
        "Linkerd control plane",
        ("identity: certificate issuance", "destination: discovery and routes", "policy: outbound watch + inbound verdict"),
        edge=GREEN,
        face=GREEN_BG,
    )
    box(
        ax,
        15.10,
        1.05,
        4.50,
        1.65,
        "Remote DPU B",
        ("same signed topology binding", "source Pod placement rechecked", "destination policy before DMA"),
        edge=BLUE,
        face=BLUE_BG,
        title_size=10.5,
        body_size=7.65,
    )

    # Local registration handshake. The workload's HELLO makes the agent spawn
    # the broker; the broker is the registering process from then on, and the
    # workload receives only the sealed attach set.
    arrow(
        ax,
        (1.45, 8.85),
        (1.45, 8.00),
        label="1  HELLO + Service",
        color=BLUE,
        label_dx=0.72,
        label_dy=0.30,
    )
    arrow(ax, (3.30, 8.00), (3.30, 8.85), color=PURPLE)
    arrow(ax, (4.10, 8.85), (4.10, 8.00), color=BLUE)
    arrow(ax, (4.90, 8.00), (4.90, 8.85), color=PURPLE)
    for label_x, label_text in (
        (3.30, "2 spawn"),
        (4.10, "4 nonce"),
        (4.90, "5 assertion"),
    ):
        ax.text(
            label_x + 0.15,
            8.425,
            label_text,
            fontsize=7.0,
            color="#626262",
            rotation=90,
            ha="center",
            va="center",
            bbox=dict(fc="white", ec="none", pad=0.5),
        )
    arrow(
        ax,
        (5.90, 9.92),
        (5.20, 9.92),
        label="3  nonce",
        color=ORANGE,
        label_dy=0.18,
    )
    arrow(
        ax,
        (5.20, 9.18),
        (5.90, 9.18),
        label="6  assert + register",
        color=BLUE,
        label_dx=0.42,
        label_dy=-0.24,
    )
    arrow(ax, (3.00, 9.80), (2.55, 9.80), color=PURPLE)
    ax.text(
        2.62,
        9.22,
        "7 sealed fds",
        fontsize=7.0,
        color="#626262",
        rotation=90,
        ha="center",
        va="center",
        bbox=dict(fc="white", ec="none", pad=0.5),
    )

    # The agent's single management hop carries all four documents. Their
    # cryptographic authorities remain distinct.
    arrow(
        ax,
        (5.20, 6.85),
        (5.90, 6.85),
        color=PURPLE,
    )
    ax.text(
        5.55,
        7.15,
        "four artifacts",
        fontsize=7.6,
        color="#626262",
        rotation=90,
        ha="center",
        va="center",
        bbox=dict(fc="white", ec="none", pad=0.7),
    )

    # The controller exchanges topology/scope and the DPU public key only with
    # the node agent. Route it above the boxes to keep the two identity gates
    # visually separate.
    line(
        ax,
        [(15.15, 10.30), (15.15, 10.48), (2.75, 10.48)],
        color=PURPLE,
        dashed=True,
    )
    arrow(ax, (2.75, 10.48), (2.75, 8.00), color=PURPLE, dashed=True)
    ax.text(
        10.0,
        10.57,
        "topology + mediated scope  /  node public-key report",
        fontsize=7.8,
        color="#626262",
        ha="center",
        bbox=dict(fc="white", ec="none", pad=0.8),
    )

    # Enforcement and the local delivery boundary.
    arrow(
        ax,
        (9.02, 8.75),
        (9.02, 8.00),
        label="verified registration",
        color=ORANGE,
        label_dy=-0.04,
    )
    arrow(
        ax,
        (9.02, 6.45),
        (9.02, 5.70),
        label="authorized target set",
        color=ORANGE,
        label_dy=-0.03,
    )
    arrow(
        ax,
        (5.90, 4.45),
        (5.20, 4.05),
        label="verdict, then SG-DMA",
        color=RED,
        label_dy=-0.18,
    )

    # TLS is end-to-end from the embedded proxy. The host relay is deliberately
    # omitted from the geometry and named on the arrow: it neither authenticates
    # as the DPU nor interprets gRPC.
    arrow(
        ax,
        (12.15, 4.78),
        (12.90, 4.78),
        label="end-to-end mTLS\nvia byte relay",
        color=GREEN,
        label_dy=0.36,
    )
    arrow(ax, (16.25, 8.00), (16.25, 8.75), label="watch cluster objects",
          color=GRAY, dashed=True, label_dx=0.80, label_dy=0.0)

    # The peer protocol, its checks and the TLS 1.3 carrier are implemented;
    # no two-node deployment has carried traffic over it yet.
    arrow(
        ax,
        (12.15, 2.08),
        (15.10, 2.08),
        label="binding / framing / custody ✓\nTLS 1.3 over RDMA: built, no 2-node run",
        color=BLUE,
        label_dy=0.42,
    )
    arrow(ax, (15.10, 1.65), (12.15, 1.65), color=BLUE)

    terms(
        ax,
        0.35,
        -2.42,
        19.25,
        2.15,
        [
            ("assertion", "node-agent-signed binding of one connection to a Pod and Service"),
            ("broker", "per-Pod host process owning the device; the Pod maps sealed memory only"),
            ("topology", "controller-signed cluster placement, peer keys and protection class"),
            ("node feed", "HMAC-signed membership or target snapshot for one node"),
            ("relay", "node-agent byte forwarding; it terminates no Linkerd TLS"),
            ("workload identity", "namespace, ServiceAccount, Pod IP from a signed binding"),
            ("verdict", "stock Linkerd inbound authorization evaluated at the DPU"),
            ("pair channel", "one authenticated/encrypted node-pair channel with bounded streams"),
            ("fail-static", "a rejected generation leaves the last fully verified one in force"),
        ],
    )

    save(fig, "control_plane")


if __name__ == "__main__":
    generate_control_plane()
