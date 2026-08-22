#!/usr/bin/env python3
"""DPUmesh mutating admission webhook.

A meshed Pod needs a fixed set of node-local facts before its first byte:
the DOCA device, the transport library, the attestation socket the node agent
listens on, its Service identity, and the two Linkerd markers that make the
workload sidecarless. Written by hand that is eight edits per Deployment.
This webhook applies all of them from one annotation.

    dpumesh.io/inject: enabled      on the Namespace, or on the Pod template

The patch is all-or-nothing. Every piece of it is load-bearing, so a Pod that
can only be half-patched is refused rather than admitted in a state that fails
later and elsewhere:

  * `linkerd.io/control-plane-ns` is what enrolls the workload in Linkerd's
    policy index — without it the Pod is simply unmeshed.
  * `config.linkerd.io/skip-inbound-ports` on the data ports is what stops the
    destination controller from advertising the endpoint as meshed. Without
    it every session ends before carrying a byte, so it is part of the data
    path and not a cosmetic default. The two are applied together or not at
    all.
  * The device, library and attestation mounts are what the transport opens at
    startup; a Pod holding some of them fails at `dpumesh_init`.

Admission runs before scheduling, so this process cannot read the node a Pod
will land on. It therefore injects a `nodeAffinity` term requiring the DPU
node label and refuses the Pod when the cluster has no such node — which is
the honest form of "refusing rather than half-injecting when the node has no
DPU". Per-node facts that vary across a heterogeneous cluster (the DOCA PCI
address) are configuration here, overridable per Pod.
"""

from __future__ import annotations

import argparse
import base64
import http.server
import json
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

# The trigger, and the Pod-level override of it. A Pod annotation wins over
# its Namespace so one workload can opt out of a meshed namespace.
INJECT_ANNOTATION = "dpumesh.io/inject"
ENABLED = {"enabled", "true", "yes"}
DISABLED = {"disabled", "false", "no"}
# Written on a patched Pod: the marker that makes a second admission pass a
# no-op rather than a duplicate patch.
INJECTED_ANNOTATION = "dpumesh.io/injected"
# Identity. The DPU resolves this Service name to the Pod's service id; a Pod
# that names none is a pure client and gets no DPUMESH_SERVICE.
SERVICE_ANNOTATION = "dpumesh.io/service"
SERVICE_LABEL = "dpumesh-service"
# The data ports, when the container's declared ports are not the whole set.
PORTS_ANNOTATION = "dpumesh.io/data-ports"
PCI_ANNOTATION = "dpumesh.io/pci-addr"
# An unmodified workload reaches the mesh through the preload shim, so the
# shim is injected by default. A workload written against the native API is
# already linked against the transport and must not be preloaded as well.
PRELOAD_ANNOTATION = "dpumesh.io/preload"
# The variable the shim is named in. An image whose entrypoint defers the
# preload past a wrapper process names its own variable instead of LD_PRELOAD,
# so that the wrapper's sockets are not intercepted too.
PRELOAD_VAR_ANNOTATION = "dpumesh.io/preload-var"
# The node label the injected affinity requires.
DPU_NODE_LABEL = "dpumesh.io/dpu"
LINKERD_CP_LABEL = "linkerd.io/control-plane-ns"
LINKERD_SKIP_ANNOTATION = "config.linkerd.io/skip-inbound-ports"

INFINIBAND_VOLUME = "dpumesh-infiniband"
LIBRARY_VOLUME = "dpumesh-library"
ATTEST_VOLUME = "dpumesh-attest"
INFINIBAND_PATH = "/dev/infiniband"

# A namespace lookup is one API call per admission without this; the trigger
# changes about as often as the namespace itself.
NAMESPACE_CACHE_TTL = 10.0


class WebhookError(RuntimeError):
    pass


class AdmissionRefused(RuntimeError):
    """A Pod that asked for injection and cannot be given all of it."""


def pointer(segment: str) -> str:
    """One JSON Pointer segment: `/` and `~` are the reserved characters."""
    return segment.replace("~", "~0").replace("/", "~1")


class KubernetesAPI:
    """Minimal in-cluster client: namespace and node reads, one CA patch."""

    def __init__(self, server: str, token_file: Path, ca_file: Path) -> None:
        self.server = server.rstrip("/")
        self.token_file = token_file
        self.context = ssl.create_default_context(cafile=str(ca_file))

    def request(self, method: str, path: str, body: bytes | None = None,
                content_type: str | None = None) -> dict[str, Any]:
        url = f"{self.server}{path}"
        headers = {"Accept": "application/json"}
        try:
            headers["Authorization"] = f"Bearer {self.token_file.read_text('ascii').strip()}"
            if content_type:
                headers["Content-Type"] = content_type
            request = urllib.request.Request(url, data=body, headers=headers, method=method)
            with urllib.request.urlopen(request, context=self.context, timeout=10) as response:
                return json.load(response)
        except (OSError, UnicodeDecodeError, urllib.error.URLError, json.JSONDecodeError) as exc:
            raise WebhookError(f"Kubernetes {method} {path} failed") from exc

    def namespace(self, name: str) -> dict[str, Any]:
        return self.request("GET", f"/api/v1/namespaces/{urllib.parse.quote(name)}")

    def nodes(self) -> list[dict[str, Any]]:
        selector = urllib.parse.quote(f"{DPU_NODE_LABEL}=true")
        document = self.request("GET", f"/api/v1/nodes?labelSelector={selector}")
        items = document.get("items")
        return items if isinstance(items, list) else []

    def set_ca_bundle(self, configuration: str, bundle: bytes) -> None:
        """Publish this process's serving CA to its own webhook registration."""
        document = self.request(
            "GET", f"/apis/admissionregistration.k8s.io/v1/mutatingwebhookconfigurations/"
                   f"{urllib.parse.quote(configuration)}")
        encoded = base64.b64encode(bundle).decode("ascii")
        patch = [
            {"op": "add",
             "path": f"/webhooks/{index}/clientConfig/caBundle",
             "value": encoded}
            for index, _ in enumerate(document.get("webhooks") or [])
        ]
        if not patch:
            raise WebhookError(f"{configuration} declares no webhook to patch")
        self.request(
            "PATCH", f"/apis/admissionregistration.k8s.io/v1/mutatingwebhookconfigurations/"
                     f"{urllib.parse.quote(configuration)}",
            json.dumps(patch).encode("utf-8"), "application/json-patch+json")


class Config:
    """Deployment-time facts the patch carries into every meshed Pod."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.linkerd_namespace = args.linkerd_namespace
        self.library_dir = args.library_dir
        self.library_soname = args.library_soname
        self.library_mount = f"{args.library_mount_dir.rstrip('/')}/{args.library_soname}"
        self.attest_dir = args.attest_dir
        self.attest_socket = args.attest_socket
        self.preload_soname = args.preload_soname
        self.preload_mount = f"{args.library_mount_dir.rstrip('/')}/{args.preload_soname}"
        self.preload_var = args.preload_var
        self.pci_addr = args.pci_addr
        self.rings_per_pod = args.rings_per_pod
        self.require_dpu_node = not args.no_node_requirement


def wants_injection(pod: dict[str, Any], namespace: dict[str, Any]) -> bool:
    """The Pod's own annotation decides; the Namespace decides when it is silent."""
    pod_annotations = (pod.get("metadata") or {}).get("annotations") or {}
    value = str(pod_annotations.get(INJECT_ANNOTATION, "")).strip().lower()
    if value in ENABLED:
        return True
    if value in DISABLED:
        return False
    namespace_annotations = (namespace.get("metadata") or {}).get("annotations") or {}
    return str(namespace_annotations.get(INJECT_ANNOTATION, "")).strip().lower() in ENABLED


def already_injected(pod: dict[str, Any]) -> bool:
    annotations = (pod.get("metadata") or {}).get("annotations") or {}
    return INJECTED_ANNOTATION in annotations


def service_identity(pod: dict[str, Any]) -> str | None:
    metadata = pod.get("metadata") or {}
    annotations = metadata.get("annotations") or {}
    labels = metadata.get("labels") or {}
    for value in (annotations.get(SERVICE_ANNOTATION), labels.get(SERVICE_LABEL)):
        if isinstance(value, str) and value.strip():
            return value.strip()
    return None


def preload_variable(pod: dict[str, Any], config: Config) -> str | None:
    """The environment variable that names the shim, or None when the workload
    declines it."""
    annotations = (pod.get("metadata") or {}).get("annotations") or {}
    if str(annotations.get(PRELOAD_ANNOTATION, "")).strip().lower() in DISABLED:
        return None
    named = annotations.get(PRELOAD_VAR_ANNOTATION)
    if isinstance(named, str) and named.strip():
        return named.strip()
    return config.preload_var


def data_ports(pod: dict[str, Any]) -> str:
    """The ports the DPU serves, which must not be advertised as meshed.

    The annotation is authoritative when present; otherwise every port the
    containers declare is one the workload listens on.
    """
    metadata = pod.get("metadata") or {}
    declared = (metadata.get("annotations") or {}).get(PORTS_ANNOTATION)
    if isinstance(declared, str) and declared.strip():
        ports = [part.strip() for part in declared.split(",") if part.strip()]
    else:
        ports = []
        for container in (pod.get("spec") or {}).get("containers") or []:
            for port in container.get("ports") or []:
                number = port.get("containerPort")
                if isinstance(number, int) and str(number) not in ports:
                    ports.append(str(number))
    for port in ports:
        if not port.isdigit() or not 1 <= int(port) <= 65535:
            raise AdmissionRefused(f"{PORTS_ANNOTATION} names {port!r}, which is not a port")
    if not ports:
        raise AdmissionRefused(
            "no data port to skip: without config.linkerd.io/skip-inbound-ports the "
            f"destination controller advertises this endpoint as meshed and every session "
            f"ends before carrying a byte. Declare a containerPort or set {PORTS_ANNOTATION}")
    return ",".join(ports)


def environment(pod: dict[str, Any], config: Config) -> list[tuple[str, str]]:
    metadata = pod.get("metadata") or {}
    annotations = metadata.get("annotations") or {}
    pci = str(annotations.get(PCI_ANNOTATION, "") or config.pci_addr).strip()
    if not pci:
        raise AdmissionRefused(
            f"no DOCA PCI address: set --pci-addr on the webhook or {PCI_ANNOTATION} on the Pod")
    variables = [("DPUMESH_PCI_ADDR", pci)]
    if config.rings_per_pod:
        variables.append(("DPUMESH_RINGS_PER_POD", str(config.rings_per_pod)))
    if config.attest_socket:
        variables.append(("DPUMESH_ATTEST_SOCKET", config.attest_socket))
    identity = service_identity(pod)
    if identity:
        variables.append(("DPUMESH_SERVICE", identity))
    variable = preload_variable(pod, config)
    if variable:
        variables.append((variable, config.preload_mount))
    return variables


def container_patch(index: int, container: dict[str, Any], config: Config,
                    variables: list[tuple[str, str]],
                    preload: bool) -> list[dict[str, Any]]:
    """Env, mounts and the privilege one container needs, added only if absent."""
    base = f"/spec/containers/{index}"
    operations: list[dict[str, Any]] = []

    existing_env = container.get("env")
    named = {entry.get("name") for entry in existing_env or [] if isinstance(entry, dict)}
    fresh = [{"name": name, "value": value} for name, value in variables if name not in named]
    if fresh:
        if existing_env is None:
            operations.append({"op": "add", "path": f"{base}/env", "value": fresh})
        else:
            operations.extend(
                {"op": "add", "path": f"{base}/env/-", "value": entry} for entry in fresh)

    existing_mounts = container.get("volumeMounts")
    mounted = {entry.get("mountPath") for entry in existing_mounts or [] if isinstance(entry, dict)}
    for entry in existing_mounts or []:
        if entry.get("mountPath") == config.library_mount and entry.get("name") != LIBRARY_VOLUME:
            raise AdmissionRefused(
                f"container {container.get('name')!r} already mounts {config.library_mount} "
                "from another volume")
    wanted = [
        {"mountPath": INFINIBAND_PATH, "name": INFINIBAND_VOLUME},
        {"mountPath": config.library_mount, "name": LIBRARY_VOLUME,
         "subPath": config.library_soname},
        {"mountPath": config.attest_dir, "name": ATTEST_VOLUME, "readOnly": True},
    ]
    if preload:
        wanted.insert(2, {"mountPath": config.preload_mount, "name": LIBRARY_VOLUME,
                          "subPath": config.preload_soname})
    fresh_mounts = [entry for entry in wanted if entry["mountPath"] not in mounted]
    if fresh_mounts:
        if existing_mounts is None:
            operations.append({"op": "add", "path": f"{base}/volumeMounts", "value": fresh_mounts})
        else:
            operations.extend(
                {"op": "add", "path": f"{base}/volumeMounts/-", "value": entry}
                for entry in fresh_mounts)

    # The transport opens the DOCA device directly, which is what this costs.
    security = container.get("securityContext")
    if security is None:
        operations.append({"op": "add", "path": f"{base}/securityContext",
                           "value": {"privileged": True}})
    elif security.get("privileged") is not True:
        operations.append({"op": "add", "path": f"{base}/securityContext/privileged",
                           "value": True})
    return operations


def volumes_patch(pod: dict[str, Any], config: Config) -> list[dict[str, Any]]:
    existing = (pod.get("spec") or {}).get("volumes")
    named = {entry.get("name") for entry in existing or [] if isinstance(entry, dict)}
    wanted = [
        {"name": INFINIBAND_VOLUME, "hostPath": {"path": INFINIBAND_PATH}},
        {"name": LIBRARY_VOLUME,
         "hostPath": {"path": config.library_dir, "type": "Directory"}},
        {"name": ATTEST_VOLUME,
         "hostPath": {"path": config.attest_dir, "type": "DirectoryOrCreate"}},
    ]
    fresh = [entry for entry in wanted if entry["name"] not in named]
    if not fresh:
        return []
    if existing is None:
        return [{"op": "add", "path": "/spec/volumes", "value": fresh}]
    return [{"op": "add", "path": "/spec/volumes/-", "value": entry} for entry in fresh]


def affinity_patch(pod: dict[str, Any]) -> list[dict[str, Any]]:
    """Require a DPU node. Admission runs before scheduling, so this is the
    only way the patch can bind the Pod to a node that can serve it."""
    term = {"matchExpressions": [
        {"key": DPU_NODE_LABEL, "operator": "In", "values": ["true"]}]}
    spec = pod.get("spec") or {}
    affinity = spec.get("affinity")
    if affinity is None:
        return [{"op": "add", "path": "/spec/affinity", "value": {
            "nodeAffinity": {"requiredDuringSchedulingIgnoredDuringExecution": {
                "nodeSelectorTerms": [term]}}}}]
    node_affinity = affinity.get("nodeAffinity")
    if node_affinity is None:
        return [{"op": "add", "path": "/spec/affinity/nodeAffinity", "value": {
            "requiredDuringSchedulingIgnoredDuringExecution": {"nodeSelectorTerms": [term]}}}]
    required = node_affinity.get("requiredDuringSchedulingIgnoredDuringExecution")
    if required is None:
        return [{"op": "add",
                 "path": "/spec/affinity/nodeAffinity/"
                         "requiredDuringSchedulingIgnoredDuringExecution",
                 "value": {"nodeSelectorTerms": [term]}}]
    terms = required.get("nodeSelectorTerms")
    if not isinstance(terms, list) or not terms:
        return [{"op": "add",
                 "path": "/spec/affinity/nodeAffinity/"
                         "requiredDuringSchedulingIgnoredDuringExecution/nodeSelectorTerms",
                 "value": [term]}]
    # Terms are OR-ed, so the requirement has to join every one of them.
    operations = []
    for index, existing in enumerate(terms):
        expressions = existing.get("matchExpressions")
        path = ("/spec/affinity/nodeAffinity/"
                f"requiredDuringSchedulingIgnoredDuringExecution/nodeSelectorTerms/{index}"
                "/matchExpressions")
        if any(expression.get("key") == DPU_NODE_LABEL for expression in expressions or []):
            continue
        if expressions is None:
            operations.append({"op": "add", "path": path, "value": term["matchExpressions"]})
        else:
            operations.append({"op": "add", "path": f"{path}/-",
                               "value": term["matchExpressions"][0]})
    return operations


def metadata_patch(pod: dict[str, Any], config: Config, ports: str) -> list[dict[str, Any]]:
    """The two Linkerd markers, applied together, and the idempotence marker."""
    metadata = pod.get("metadata") or {}
    operations: list[dict[str, Any]] = []

    labels = metadata.get("labels")
    if labels is None:
        operations.append({"op": "add", "path": "/metadata/labels",
                           "value": {LINKERD_CP_LABEL: config.linkerd_namespace}})
    elif labels.get(LINKERD_CP_LABEL) != config.linkerd_namespace:
        operations.append({"op": "add",
                           "path": f"/metadata/labels/{pointer(LINKERD_CP_LABEL)}",
                           "value": config.linkerd_namespace})

    annotations = metadata.get("annotations")
    added = {LINKERD_SKIP_ANNOTATION: ports, INJECTED_ANNOTATION: config.library_soname}
    if annotations is None:
        operations.append({"op": "add", "path": "/metadata/annotations", "value": added})
    else:
        for key, value in added.items():
            if annotations.get(key) != value:
                operations.append({"op": "add", "path": f"/metadata/annotations/{pointer(key)}",
                                   "value": value})
    return operations


def build_patch(pod: dict[str, Any], config: Config) -> list[dict[str, Any]]:
    """The whole patch, or an AdmissionRefused. Never a part of it."""
    containers = (pod.get("spec") or {}).get("containers")
    if not containers:
        raise AdmissionRefused("a meshed Pod must declare at least one container")
    ports = data_ports(pod)
    variables = environment(pod, config)
    operations: list[dict[str, Any]] = []
    preload = preload_variable(pod, config) is not None
    for index, container in enumerate(containers):
        operations.extend(container_patch(index, container, config, variables, preload))
    operations.extend(volumes_patch(pod, config))
    if config.require_dpu_node:
        operations.extend(affinity_patch(pod))
    operations.extend(metadata_patch(pod, config, ports))
    return operations


def ensure_serving_certificate(cert_path: Path, key_path: Path, dns_names: list[str]) -> None:
    """Generate the serving certificate this process presents, if it has none.

    The API server verifies it against the `caBundle` the registration carries,
    and this process is what writes that bundle, so a self-signed certificate is
    the whole chain. Regenerating it on restart is fine because the bundle is
    republished in the same startup.
    """
    if cert_path.exists() and key_path.exists():
        return
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    import datetime

    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, dns_names[0])])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in dns_names]),
                       critical=False)
        .sign(key, hashes.SHA256())
    )
    cert_path.parent.mkdir(parents=True, exist_ok=True)
    cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()))
    key_path.chmod(0o600)


def response(uid: str, allowed: bool, message: str | None = None,
             patch: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    body: dict[str, Any] = {"uid": uid, "allowed": allowed}
    if message:
        body["status"] = {"message": message}
    if patch:
        body["patchType"] = "JSONPatch"
        body["patch"] = base64.b64encode(json.dumps(patch).encode("utf-8")).decode("ascii")
    return {"apiVersion": "admission.k8s.io/v1", "kind": "AdmissionReview", "response": body}


class Injector:
    """Admission decisions, and the cluster reads two of them need."""

    def __init__(self, api: KubernetesAPI | None, config: Config) -> None:
        self.api = api
        self.config = config
        self.lock = threading.Lock()
        self.namespaces: dict[str, tuple[float, dict[str, Any]]] = {}

    def namespace(self, name: str) -> dict[str, Any]:
        """The Namespace carrying the trigger, or an empty one when it cannot
        be read.

        A lookup that fails leaves the Pod's own annotation to decide. Refusing
        instead would refuse every Pod in the namespace, including the ones that
        never asked to be meshed — and an unpatched Pod is a working Pod, which
        is the same trade `failurePolicy: Ignore` makes.
        """
        if self.api is None or not name:
            return {}
        now = time.monotonic()
        with self.lock:
            cached = self.namespaces.get(name)
            if cached and now - cached[0] < NAMESPACE_CACHE_TTL:
                return cached[1]
        try:
            document = self.api.namespace(name)
        except WebhookError as exc:
            sys.stderr.write(f"webhook: namespace {name} unreadable ({exc}); "
                             "the Pod's own annotation decides\n")
            return {}
        with self.lock:
            self.namespaces[name] = (now, document)
        return document

    def dpu_node_exists(self) -> bool:
        return self.api is None or bool(self.api.nodes())

    def review(self, review: dict[str, Any]) -> dict[str, Any]:
        request = review.get("request") or {}
        uid = str(request.get("uid", ""))
        pod = request.get("object") or {}
        name = request.get("namespace") or ""
        try:
            if not wants_injection(pod, self.namespace(name)) or already_injected(pod):
                return response(uid, True)
            if self.config.require_dpu_node and not self.dpu_node_exists():
                raise AdmissionRefused(
                    f"no node carries {DPU_NODE_LABEL}=true, so this Pod cannot be meshed. "
                    "Label the DPU nodes or remove the injection annotation")
            return response(uid, True, patch=build_patch(pod, self.config))
        except AdmissionRefused as exc:
            return response(uid, False, f"dpumesh injection refused: {exc}")
        except WebhookError as exc:
            return response(uid, False, f"dpumesh injection could not be decided: {exc}")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    injector: Injector

    def log_message(self, format: str, *args: Any) -> None:
        sys.stderr.write("webhook: " + format % args + "\n")

    def reply(self, code: int, body: bytes, content_type: str = "application/json") -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if urllib.parse.urlparse(self.path).path == "/healthz":
            self.reply(200, b"ok\n", "text/plain")
        else:
            self.reply(404, b"not found\n", "text/plain")

    def do_POST(self) -> None:
        if urllib.parse.urlparse(self.path).path != "/mutate":
            self.reply(404, b"not found\n", "text/plain")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            review = json.loads(self.rfile.read(length))
        except (ValueError, json.JSONDecodeError):
            self.reply(400, b'{"error":"malformed AdmissionReview"}')
            return
        self.reply(200, json.dumps(self.injector.review(review)).encode("utf-8"))


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="DPUmesh mutating admission webhook")
    parser.add_argument("--listen-port", type=int, default=8443)
    parser.add_argument("--tls-cert", type=Path)
    parser.add_argument("--tls-key", type=Path)
    parser.add_argument("--linkerd-namespace", default="linkerd")
    parser.add_argument("--library-dir", default="/opt/dpumesh/lib",
                        help="host directory holding the transport library")
    parser.add_argument("--library-soname", default="libdpumesh.so.5")
    parser.add_argument("--library-mount-dir", default="/usr/local/lib")
    parser.add_argument("--preload-soname", default="libdmesh_preload.so")
    parser.add_argument("--preload-var", default="LD_PRELOAD")
    parser.add_argument("--attest-dir", default="/run/dpumesh")
    parser.add_argument("--attest-socket", default="/run/dpumesh/attest.sock")
    parser.add_argument("--pci-addr", default="")
    parser.add_argument("--rings-per-pod", type=int, default=0)
    parser.add_argument("--no-node-requirement", action="store_true",
                        help="skip the DPU node affinity and its cluster check")
    parser.add_argument("--api-server", default="https://kubernetes.default.svc")
    parser.add_argument("--token-file", type=Path,
                        default=Path("/var/run/secrets/kubernetes.io/serviceaccount/token"))
    parser.add_argument("--ca-file", type=Path,
                        default=Path("/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"))
    parser.add_argument("--publish-ca-bundle", default="",
                        help="MutatingWebhookConfiguration to write --tls-cert into")
    parser.add_argument("--service-dns", default="",
                        help="comma-separated names the generated certificate is valid for")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    config = Config(args)
    api = None
    if args.token_file.exists():
        api = KubernetesAPI(args.api_server, args.token_file, args.ca_file)
    if args.tls_cert and args.tls_key and args.service_dns:
        ensure_serving_certificate(
            args.tls_cert, args.tls_key,
            [name.strip() for name in args.service_dns.split(",") if name.strip()])
    if args.publish_ca_bundle:
        if api is None:
            raise WebhookError("--publish-ca-bundle needs in-cluster credentials")
        api.set_ca_bundle(args.publish_ca_bundle, args.tls_cert.read_bytes())

    Handler.injector = Injector(api, config)
    server = Server(("0.0.0.0", args.listen_port), Handler)
    if args.tls_cert and args.tls_key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(args.tls_cert), str(args.tls_key))
        server.socket = context.wrap_socket(server.socket, server_side=True)
    sys.stderr.write(f"webhook: listening on :{args.listen_port}\n")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
