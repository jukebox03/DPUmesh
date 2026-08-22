"""Contract for the DPUmesh admission webhook.

The patch is judged by applying it, not by reading it: a JSON Pointer this
module escapes wrongly produces a patch Kubernetes rejects at admission time,
which is exactly the failure a shape assertion would miss.
"""

import importlib.util
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "dpumesh_webhook", ROOT / "controller" / "dpumesh_webhook.py"
)
assert SPEC and SPEC.loader
webhook = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(webhook)


def config(**overrides):
    argv = ["--pci-addr=94:00.0", "--library-dir=/opt/dpumesh/lib", "--rings-per-pod=2"]
    argv += [f"--{key.replace('_', '-')}={value}" for key, value in overrides.items()]
    return webhook.Config(webhook.parse_args(argv))


def pod(annotations=None, labels=None, containers=None, spec=None):
    body = {
        "metadata": {"name": "workload", "namespace": "test-bench"},
        "spec": {"containers": containers if containers is not None else [
            {"name": "app", "image": "app:latest", "ports": [{"containerPort": 9091}]}
        ]},
    }
    if annotations:
        body["metadata"]["annotations"] = annotations
    if labels:
        body["metadata"]["labels"] = labels
    if spec:
        body["spec"].update(spec)
    return body


def namespace(annotations=None):
    return {"metadata": {"name": "test-bench", "annotations": annotations or {}}}


def apply_patch(document, operations):
    """The subset of RFC 6902 this webhook emits: `add`, with `-` on arrays."""
    result = json.loads(json.dumps(document))
    for operation in operations:
        assert operation["op"] == "add", operation
        parts = [part.replace("~1", "/").replace("~0", "~")
                 for part in operation["path"].split("/")[1:]]
        target = result
        for part in parts[:-1]:
            target = target[int(part)] if isinstance(target, list) else target[part]
        last = parts[-1]
        if isinstance(target, list):
            if last == "-":
                target.append(operation["value"])
            else:
                target.insert(int(last), operation["value"])
        else:
            target[last] = operation["value"]
    return result


def review(body, namespace_document, injector_config=None, nodes=True):
    class FakeAPI:
        def namespace(self, name):
            return namespace_document

        def nodes(self):
            return [{"metadata": {"name": "rapids4"}}] if nodes else []

    injector = webhook.Injector(FakeAPI(), injector_config or config())
    return injector.review({
        "request": {"uid": "u1", "namespace": "test-bench", "object": body}
    })


def patch_of(response):
    assert response["response"]["allowed"], response["response"].get("status")
    import base64
    encoded = response["response"].get("patch")
    return json.loads(base64.b64decode(encoded)) if encoded else []


def test_untriggered_pod_is_untouched():
    response = review(pod(), namespace())
    assert response["response"]["allowed"]
    assert "patch" not in response["response"]


def test_namespace_annotation_triggers_injection():
    response = review(pod(), namespace({"dpumesh.io/inject": "enabled"}))
    assert patch_of(response), "an annotated namespace must produce a patch"


def test_pod_annotation_overrides_its_namespace():
    off = review(pod({"dpumesh.io/inject": "disabled"}),
                 namespace({"dpumesh.io/inject": "enabled"}))
    assert "patch" not in off["response"]
    on = review(pod({"dpumesh.io/inject": "enabled"}), namespace())
    assert patch_of(on)


def test_patch_carries_both_linkerd_markers():
    body = pod({"dpumesh.io/inject": "enabled"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    labels = patched["metadata"]["labels"]
    annotations = patched["metadata"]["annotations"]
    # One without the other is the failure this pairing exists to prevent.
    assert labels["linkerd.io/control-plane-ns"] == "linkerd"
    assert annotations["config.linkerd.io/skip-inbound-ports"] == "9091"


def test_patch_carries_the_node_local_facts():
    body = pod({"dpumesh.io/inject": "enabled"}, labels={"dpumesh-service": "echo-dpumesh"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    container = patched["spec"]["containers"][0]
    environment = {entry["name"]: entry["value"] for entry in container["env"]}
    assert environment["DPUMESH_PCI_ADDR"] == "94:00.0"
    assert environment["DPUMESH_RINGS_PER_POD"] == "2"
    assert environment["DPUMESH_SERVICE"] == "echo-dpumesh"
    assert container["securityContext"]["privileged"] is True
    mounts = {entry["mountPath"]: entry for entry in container["volumeMounts"]}
    assert mounts["/dev/infiniband"]["name"] == "dpumesh-infiniband"
    assert mounts["/usr/local/lib/libdmesh_preload.so"]["subPath"] == "libdmesh_preload.so"
    assert mounts["/usr/local/lib/libdpumesh.so.5"]["subPath"] == "libdpumesh.so.5"
    assert mounts["/run/dpumesh"]["readOnly"] is True
    volumes = {entry["name"]: entry for entry in patched["spec"]["volumes"]}
    assert volumes["dpumesh-library"]["hostPath"]["path"] == "/opt/dpumesh/lib"
    assert volumes["dpumesh-attest"]["hostPath"]["type"] == "DirectoryOrCreate"


def test_an_unmodified_workload_is_preloaded():
    body = pod({"dpumesh.io/inject": "enabled"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    environment = {entry["name"]: entry["value"]
                   for entry in patched["spec"]["containers"][0]["env"]}
    assert environment["LD_PRELOAD"] == "/usr/local/lib/libdmesh_preload.so"


def test_a_native_workload_declines_the_shim():
    # A workload linked against the transport must not also preload it.
    body = pod({"dpumesh.io/inject": "enabled", "dpumesh.io/preload": "disabled"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    container = patched["spec"]["containers"][0]
    names = {entry["name"] for entry in container["env"]}
    mounts = {entry["mountPath"] for entry in container["volumeMounts"]}
    assert "LD_PRELOAD" not in names
    assert "/usr/local/lib/libdmesh_preload.so" not in mounts


def test_an_image_may_name_its_own_preload_variable():
    body = pod({"dpumesh.io/inject": "enabled",
                "dpumesh.io/preload-var": "NUMA_TARGET_LD_PRELOAD"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    environment = {entry["name"]: entry["value"]
                   for entry in patched["spec"]["containers"][0]["env"]}
    assert environment["NUMA_TARGET_LD_PRELOAD"] == "/usr/local/lib/libdmesh_preload.so"
    assert "LD_PRELOAD" not in environment


def test_a_client_without_identity_gets_no_service():
    body = pod({"dpumesh.io/inject": "enabled"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    names = {entry["name"] for entry in patched["spec"]["containers"][0]["env"]}
    assert "DPUMESH_SERVICE" not in names


def test_identity_annotation_beats_the_label():
    body = pod({"dpumesh.io/inject": "enabled", "dpumesh.io/service": "chosen"},
               labels={"dpumesh-service": "labelled"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    environment = {entry["name"]: entry["value"]
                   for entry in patched["spec"]["containers"][0]["env"]}
    assert environment["DPUMESH_SERVICE"] == "chosen"


def test_a_pod_with_no_data_port_is_refused():
    body = pod({"dpumesh.io/inject": "enabled"},
               containers=[{"name": "app", "image": "app:latest"}])
    response = review(body, namespace())
    assert not response["response"]["allowed"]
    assert "skip-inbound-ports" in response["response"]["status"]["message"]


def test_declared_ports_may_be_overridden():
    body = pod({"dpumesh.io/inject": "enabled", "dpumesh.io/data-ports": "9091,9095"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    assert patched["metadata"]["annotations"][
        "config.linkerd.io/skip-inbound-ports"] == "9091,9095"


def test_a_bad_port_is_refused():
    body = pod({"dpumesh.io/inject": "enabled", "dpumesh.io/data-ports": "http"})
    response = review(body, namespace())
    assert not response["response"]["allowed"]


def test_injection_is_idempotent():
    body = pod({"dpumesh.io/inject": "enabled"})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    second = review(patched, namespace())
    assert second["response"]["allowed"]
    assert "patch" not in second["response"], "a patched Pod must not be patched twice"


def test_existing_environment_is_not_duplicated():
    body = pod({"dpumesh.io/inject": "enabled"}, containers=[{
        "name": "app", "image": "app:latest", "ports": [{"containerPort": 9091}],
        "env": [{"name": "DPUMESH_PCI_ADDR", "value": "03:00.0"}],
    }])
    patched = apply_patch(body, patch_of(review(body, namespace())))
    values = [entry for entry in patched["spec"]["containers"][0]["env"]
              if entry["name"] == "DPUMESH_PCI_ADDR"]
    assert len(values) == 1 and values[0]["value"] == "03:00.0"


def test_every_container_is_patched():
    body = pod({"dpumesh.io/inject": "enabled"}, containers=[
        {"name": "a", "image": "a", "ports": [{"containerPort": 9091}]},
        {"name": "b", "image": "b"},
    ])
    patched = apply_patch(body, patch_of(review(body, namespace())))
    for container in patched["spec"]["containers"]:
        assert container["securityContext"]["privileged"] is True
        assert len(container["volumeMounts"]) == 4


def test_a_cluster_with_no_dpu_node_refuses():
    body = pod({"dpumesh.io/inject": "enabled"})
    response = review(body, namespace(), nodes=False)
    assert not response["response"]["allowed"]
    assert "dpumesh.io/dpu" in response["response"]["status"]["message"]


def test_node_requirement_joins_every_existing_term():
    body = pod({"dpumesh.io/inject": "enabled"}, spec={"affinity": {"nodeAffinity": {
        "requiredDuringSchedulingIgnoredDuringExecution": {"nodeSelectorTerms": [
            {"matchExpressions": [{"key": "zone", "operator": "In", "values": ["a"]}]},
            {"matchExpressions": [{"key": "zone", "operator": "In", "values": ["b"]}]},
        ]}}}})
    patched = apply_patch(body, patch_of(review(body, namespace())))
    terms = patched["spec"]["affinity"]["nodeAffinity"][
        "requiredDuringSchedulingIgnoredDuringExecution"]["nodeSelectorTerms"]
    # Terms are OR-ed, so a requirement missing from one of them is not a
    # requirement at all.
    for term in terms:
        assert any(expression["key"] == "dpumesh.io/dpu"
                   for expression in term["matchExpressions"])


def test_a_conflicting_library_mount_is_refused():
    body = pod({"dpumesh.io/inject": "enabled"}, containers=[{
        "name": "app", "image": "app:latest", "ports": [{"containerPort": 9091}],
        "volumeMounts": [{"mountPath": "/usr/local/lib/libdpumesh.so.5", "name": "other"}],
    }])
    response = review(body, namespace())
    assert not response["response"]["allowed"]


def test_an_unreadable_namespace_does_not_refuse_uninvolved_pods():
    # Refusing here would refuse every Pod in the namespace, including the ones
    # that never asked to be meshed.
    class BrokenAPI:
        def namespace(self, name):
            raise webhook.WebhookError("the API server said no")

        def nodes(self):
            return [{"metadata": {"name": "rapids4"}}]

    injector = webhook.Injector(BrokenAPI(), config())
    plain = injector.review({"request": {"uid": "u", "namespace": "test-bench",
                                         "object": pod()}})
    assert plain["response"]["allowed"]
    assert "patch" not in plain["response"]
    # A Pod that asked for it is still decided by its own annotation.
    asked = injector.review({"request": {"uid": "u", "namespace": "test-bench",
                                         "object": pod({"dpumesh.io/inject": "enabled"})}})
    assert asked["response"]["allowed"] and asked["response"].get("patch")


def test_missing_pci_address_is_refused():
    body = pod({"dpumesh.io/inject": "enabled"})
    bare = webhook.Config(webhook.parse_args(["--library-dir=/opt/dpumesh/lib"]))
    response = review(body, namespace(), injector_config=bare)
    assert not response["response"]["allowed"]


def main():
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
        print(f"  ok  {test.__name__}")
    print(f"dpumesh_webhook_test: {len(tests)} passed")


if __name__ == "__main__":
    main()
