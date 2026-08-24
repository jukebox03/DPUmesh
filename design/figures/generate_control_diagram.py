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
    fig, ax = setup_figure(19.8, 13.15, (0, 19.8), (-2.15, 11.0))
    ax.text(0.1, 10.62, "DPUmesh control plane — who states what, and who signs it",
            fontsize=17, ha="left")

    # Bands. The gateway is a host DaemonSet, but it sits between the DPU and
    # the control services, so it gets its own band rather than a line that
    # crosses the picture twice.
    for xsep in (6.05, 12.35, 15.75):
        ax.plot([xsep, xsep], [0.45, 9.75], color="#ddddda", lw=1, ls=(0, (2, 3)))
    ax.text(2.90, 9.98, "HOST NODE", fontsize=11, color="#555555", ha="center")
    ax.text(9.10, 9.98, "BLUEFIELD DPU", fontsize=11, color="#555555", ha="center")
    ax.text(14.05, 9.98, "HOST NETWORK", fontsize=11, color="#555555", ha="center")
    ax.text(17.90, 9.98, "KUBERNETES", fontsize=11, color="#555555", ha="center")

    # ---- host ----
    box(ax, 0.35, 8.50, 5.10, 1.35, "Service Pod",
        ("application + DPUmesh client",
         "relays a grant it cannot alter"),
        edge=BLUE, face=BLUE_BG)
    box(ax, 0.35, 6.30, 5.10, 1.55, "Node agent DaemonSet",
        ("SO_PEERCRED + peer cgroup -> Pod",
         "reads Kubernetes, signs claims",
         "root-owned; holds the keyring"),
        edge=PURPLE, face=PURPLE_BG)
    box(ax, 0.35, 4.35, 5.10, 1.30, "Service registry publisher",
        ("Service ClusterIP + ready endpoints",
         "signed, monotonic generations"),
        edge=PURPLE, face=PURPLE_BG)
    box(ax, 0.35, 2.40, 5.10, 1.30, "Identity renewal agent",
        ("projected token, trust roots",
         "private key and CSR"),
        edge=PURPLE, face=PURPLE_BG)

    # ---- DPU ----
    box(ax, 6.45, 8.50, 5.30, 1.35, "Registration",
        ("verifies key id, nonce, issuer,",
         "expiry and the exact Service"),
        edge=ORANGE, face=ORANGE_BG)
    box(ax, 6.45, 6.05, 5.30, 1.35, "Service snapshot",
        ("targets and ready endpoints",
         "adopted only if strictly newer"),
        edge=ORANGE, face=ORANGE_BG)
    box(ax, 6.45, 4.35, 5.30, 1.30, "Session bound to that registration",
        ("refuses an address this generation",
         "places in another Service"),
        edge=GREEN, face=GREEN_BG)
    box(ax, 6.45, 2.40, 5.30, 1.45, "Embedded linkerd2-proxy",
        ("outbound half only",
         "one shared dpumesh-dpu identity"),
        edge=GREEN, face=GREEN_BG)
    box(ax, 6.45, 0.55, 2.45, 1.30, "Identity material",
        ("root-only files,", "atomic rename"),
        edge=ORANGE, face=ORANGE_BG, title_size=10)
    box(ax, 9.30, 0.55, 2.45, 1.30, "Backend Pod",
        ("registered on", "this same DPU"),
        edge=RED, face=RED_BG, title_size=10)

    # ---- gateway ----
    box(ax, 12.70, 3.35, 2.70, 3.55, "Gateway DaemonSet",
        ("host network",
         "carries bytes;",
         "terminates no",
         "mesh identity;",
         "reads no gRPC"),
        edge=PURPLE, face=PURPLE_BG, title_size=10.0)

    # ---- Kubernetes ----
    box(ax, 16.05, 7.10, 3.70, 0.95, "Pod and Service API",
        edge=GRAY, face=GRAY_BG, title_size=10.5, count="cluster objects")
    box(ax, 16.05, 5.95, 3.70, 1.00, "Linkerd Identity",
        edge=GREEN, face=GREEN_BG, title_size=10.5)
    box(ax, 16.05, 4.55, 3.70, 1.00, "Linkerd Policy",
        edge=GREEN, face=GREEN_BG, title_size=10.5)
    box(ax, 16.05, 3.15, 3.70, 1.00, "Linkerd Destination",
        edge=GREEN, face=GREEN_BG, title_size=10.5)

    # ---- the registration handshake ----
    arrow(ax, (6.45, 9.48), (5.45, 9.48), label="1. fresh nonce", color=ORANGE)
    arrow(ax, (1.55, 8.50), (1.55, 7.85), label="2. nonce + requested Service",
          color=BLUE, label_dx=0.55)
    arrow(ax, (4.25, 7.85), (4.25, 8.50), label="3. signed grant", color=PURPLE,
          label_dx=0.05)
    arrow(ax, (5.45, 8.82), (6.45, 8.82), label="4. grant + POD_REGISTER",
          color=BLUE, label_dy=-0.22)

    # The claims the agent signs come from here, not from the Pod. The line runs
    # in the gap between Registration and the Service snapshot, so it crosses the
    # picture without crossing a box.
    arrow(ax, (16.05, 7.55), (5.45, 7.55), color="#b9b9b4", dashed=True, lw=1.3,
          label="authoritative Pod and Service objects", label_dy=0.23)

    # ---- feeds ----
    arrow(ax, (5.45, 7.35), (7.10, 8.50), label="signed node\nmembership feed",
          color=PURPLE, label_dx=-0.55, label_dy=0.30)
    arrow(ax, (5.45, 5.25), (6.45, 6.55), label="signed Service\ntarget feed",
          color=PURPLE, label_dx=-0.30, label_dy=0.40)
    arrow(ax, (5.45, 2.85), (6.45, 1.70), label="atomic update",
          color=PURPLE, label_dy=-0.30)

    # ---- what the DPU does with them ----
    arrow(ax, (9.10, 8.50), (9.10, 7.40), label="admitted registration",
          color=ORANGE, label_dy=-0.02)
    arrow(ax, (9.10, 6.05), (9.10, 5.65), label="the addresses a session may dial",
          color=ORANGE, label_dy=0.13)
    arrow(ax, (9.10, 4.35), (9.10, 3.85), color=GREEN)
    arrow(ax, (7.15, 1.85), (7.15, 2.40), label="credential watch",
          color=ORANGE, label_dx=0.85, label_dy=0.0)
    arrow(ax, (10.55, 2.40), (10.55, 1.85), label="policy applied,\nthen DMA",
          color=RED, label_dx=0.80, label_dy=0.0)

    # ---- control connections ----
    arrow(ax, (11.75, 3.55), (12.70, 4.55), label="end-to-end mTLS",
          color=GREEN, label_dx=0.35, label_dy=-0.28)
    arrow(ax, (15.40, 6.45), (16.05, 6.45), color=GREEN)
    arrow(ax, (15.40, 5.05), (16.05, 5.05), color=GREEN)
    arrow(ax, (15.40, 3.65), (16.05, 3.65), color=GREEN)

    terms(
        ax,
        0.35,
        -2.05,
        19.10,
        1.80,
        [
            ("grant", "signed statement of a Pod's identity and authorized Service"),
            ("feed", "a file the DPU reads for authoritative data"),
            ("generation", "one version of a feed, installed whole by atomic rename"),
            ("keyring", "root-only keys that sign grants and both feeds"),
            ("workload", "the Linkerd identifier built from the grant's signed claims"),
            ("session", "one client connection and the outbound stack built for it"),
            ("snapshot", "the Service each address the held generation names"),
            ("gateway", "carries the DPU's control connections without reading them"),
        ],
    )

    save(fig, "control_plane")


if __name__ == "__main__":
    generate_control_plane()
