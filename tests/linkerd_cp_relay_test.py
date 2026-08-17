import importlib.util
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "linkerd_cp_relay", ROOT / "bench" / "linkerd_cp_relay.py"
)
assert SPEC and SPEC.loader
relay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = relay
SPEC.loader.exec_module(relay)


class FakeAPI(relay.KubernetesAPI):
    def __init__(self, documents):
        self.documents = documents

    def get(self, path):
        return self.documents[path]


def main():
    static = relay.route("dst=192.168.100.1:28086->10.0.0.1:8086")
    assert static.target_host == "10.0.0.1"
    dynamic = relay.kube_route(
        "policy=192.168.100.1:28087->linkerd/linkerd-policy:8090"
    )
    assert dynamic.target_namespace == "linkerd"
    assert dynamic.target_service == "linkerd-policy"

    api = FakeAPI(
        {
            "/api/v1/namespaces/linkerd/services/linkerd-dst": {
                "spec": {"clusterIP": "10.96.0.10"}
            },
            "/api/v1/namespaces/linkerd/services/linkerd-policy": {
                "spec": {"clusterIP": "None"}
            },
            "/api/v1/namespaces/linkerd/endpoints/linkerd-policy": {
                "subsets": [{"addresses": [{"ip": "10.244.0.9"}]}]
            },
        }
    )
    assert api.resolve("linkerd", "linkerd-dst") == "10.96.0.10"
    assert api.resolve("linkerd", "linkerd-policy") == "10.244.0.9"
    print("linkerd_cp_relay_test: PASS")


if __name__ == "__main__":
    main()
