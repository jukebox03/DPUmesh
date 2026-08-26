# Build the host transport library and benchmark/validator binaries. DPU ARM and
# DPA components use doca/meson.build. Targets: all, lib, bench, go, clean.

CC      ?= gcc
DOCA_PKGS := doca-common doca-comch doca-dpa
DOCA_CFLAGS := $(shell pkg-config --cflags $(DOCA_PKGS))
DOCA_LIBS   := $(shell pkg-config --libs   $(DOCA_PKGS))
CRYPTO_LIBS := $(shell pkg-config --libs libcrypto)
TLS_LIBS    := $(shell pkg-config --libs libssl)
RDMA_LIBS   := $(shell pkg-config --libs librdmacm libibverbs)
DOCA_LIBDIR := $(shell pkg-config --variable=libdir doca-common)
FLEXIO_LIBDIR := /opt/mellanox/flexio/lib

BUILD   := build
LIBDIR  := $(BUILD)/lib
BINDIR  := $(BUILD)/bin
TESTDIR := $(BUILD)/test

# -Iinclude → <dpumesh/...> ; -I. → the "doca/..." includes from src/dmesh_core.c
# -Ilinkerd/include → <dmesh_l7.h>, the L7 adapter contract the proxy compiles against
# -Wextra minus the categories that only fire on deliberate patterns: callback
# parameters fixed by DOCA's signatures, ring index arithmetic, and port-table
# bound checks a uint16_t index already satisfies.
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS  := -O2 -g $(WARNFLAGS) -fPIC -DDOCA_ALLOW_EXPERIMENTAL_API -Iinclude -I. -Ilinkerd/include $(DOCA_CFLAGS)

# Runtime search paths. In a container everything is copied to /usr/local/lib;
# for a local build we also point at the in-tree lib dir and the DOCA libs.
RPATHS  := -Wl,-rpath,/usr/local/lib \
           -Wl,-rpath,$(abspath $(LIBDIR)) \
           -Wl,-rpath,$(DOCA_LIBDIR) \
           -Wl,-rpath,$(FLEXIO_LIBDIR)

# ---- host transport library --------------------------------------------------
LIB_SRCS := \
	src/dmesh_core.c \
	src/dmesh_api.c \
	src/dmesh_attest.c \
	src/dmesh_resolve.c \
	doca/common.c \
	doca/object.c \
	doca/buffer.c \
	doca/ring.c \
	doca/comch_common.c \
	doca/comch_client.c \
	doca/comch_server.c \
	doca/comch_msgq.c \
	doca/workload_grant.c \
	doca/pod_membership.c \
	doca/topology.c \
	doca/peer_channel.c \
	doca/control_scope.c \
	doca/dpa.c
LIB_HDRS := $(shell find include src doca -name '*.h' 2>/dev/null)

# ABI major. BUMP IT whenever the public ABI changes incompatibly — a field added to
# dmesh_event_t / dmesh_qp_t / dmesh_channel_t, a reorder, a signature change. The SONAME
# identifies incompatible public layouts at load time.
ABI_MAJOR := 5
LIB      := $(LIBDIR)/libdpumesh.so.$(ABI_MAJOR)
LIB_LINK := $(LIBDIR)/libdpumesh.so

# ---- consumers of the library ------------------------------------------------
# dmesh_* API clients (socket/epoll façade over dmesh.h)
DMESH_BINS := bench_dpumesh echo_dpumesh loopback_dpumesh verbs_dpumesh
bench_dpumesh_SRC    := bench/apps/bench_dpumesh.c
bench_dpumesh_LIBS   := -lm
echo_dpumesh_SRC     := bench/apps/echo_dpumesh.c
loopback_dpumesh_SRC := bench/validators/loopback_dpumesh.c
verbs_dpumesh_SRC    := bench/validators/verbs_dpumesh.c

# LD_PRELOAD shim (interposes libc sockets → dmesh) + its vanilla-TCP validators
PRELOAD := $(LIBDIR)/libdmesh_preload.so
POSIX_BINS := tcp_echo tcp_client preload_runner bench_sock echo_sock http1_bench http1_echo  # preload inputs, no dmesh link
tcp_echo_SRC       := bench/validators/tcp_echo.c
tcp_client_SRC     := bench/validators/tcp_client.c
preload_runner_SRC := bench/validators/preload_runner.c
# bench_sock/echo_sock are ordinary POSIX applications; the preload deployment
# interposes their data sockets without changing the programs.
bench_sock_SRC     := bench/apps/bench_sock.c
echo_sock_SRC      := bench/apps/echo_sock.c

.PHONY: all lib bench test test-hostfree clean dirs
all: lib bench

# Header dependencies for the library and all consumers.
DEPDIR  := $(BUILD)/dep
DEPFLAGS = -MMD -MP -MF $(DEPDIR)/$(@F).d

dirs:
	@mkdir -p $(LIBDIR) $(BINDIR) $(DEPDIR) $(TESTDIR)

lib: dirs $(LIB) $(LIB_LINK)
$(LIB): $(LIB_SRCS) $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -shared -Wl,-soname,libdpumesh.so.$(ABI_MAJOR) -o $@ $(LIB_SRCS) \
		$(DOCA_LIBS) $(CRYPTO_LIBS) -lpthread $(RPATHS)
	@echo "  -> $@"

# The unversioned name is the LINKER's entry point only (-ldpumesh). What a binary
# records as DT_NEEDED is the SONAME above, so runtime resolution never goes through it.
$(LIB_LINK): $(LIB)
	@ln -sf $(notdir $(LIB)) $@

bench: lib $(addprefix $(BINDIR)/,$(DMESH_BINS)) $(PRELOAD) $(addprefix $(BINDIR)/,$(POSIX_BINS))

# Focused host-only contract tests. Function-section GC lets each test link the
# production source that owns the state machine without constructing DOCA hardware.
$(TESTDIR)/native_api_contract_test: tests/native_api_contract_test.c src/dmesh_api.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/native_api_contract_test.c src/dmesh_api.c

$(TESTDIR)/native_control_state_test: tests/native_control_state_test.c doca/comch_server.c doca/workload_grant.c doca/topology.c doca/control_scope.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/native_control_state_test.c doca/comch_server.c doca/workload_grant.c \
		doca/topology.c doca/control_scope.c \
		$(DOCA_LIBS) $(CRYPTO_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/workload_grant_test: tests/workload_grant_test.c doca/workload_grant.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -o $@ tests/workload_grant_test.c doca/workload_grant.c \
		$(DOCA_LIBS) $(CRYPTO_LIBS) $(RPATHS)

$(TESTDIR)/pod_membership_test: tests/pod_membership_test.c doca/pod_membership.c doca/comch_server.c doca/workload_grant.c doca/topology.c doca/control_scope.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/pod_membership_test.c doca/pod_membership.c \
		doca/comch_server.c doca/workload_grant.c doca/topology.c doca/control_scope.c \
		$(DOCA_LIBS) $(CRYPTO_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/native_tx_batch_policy_test: tests/native_tx_batch_policy_test.c src/dmesh_core.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/native_tx_batch_policy_test.c $(DOCA_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/native_writable_test: tests/native_writable_test.c src/dmesh_core.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/native_writable_test.c $(DOCA_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/preload_api_contract_test: tests/preload_api_contract_test.c src/dmesh_preload.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -o $@ tests/preload_api_contract_test.c -lpthread -ldl

$(TESTDIR)/l4_pin_policy_test: tests/l4_pin_policy_test.c doca/dpu_proxy.h | dirs
	$(CC) $(CFLAGS) -I. -o $@ tests/l4_pin_policy_test.c

$(TESTDIR)/lb_policy_test: tests/lb_policy_test.c doca/dpu_worker.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/lb_policy_test.c $(DOCA_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/proxy_lane_queue_test: tests/proxy_lane_queue_test.c doca/dpu_proxy.c doca/peer_channel.c doca/peer_transport.c doca/peer_tls.c doca/topology.c doca/workload_grant.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/proxy_lane_queue_test.c doca/peer_channel.c \
		doca/peer_transport.c doca/peer_tls.c doca/topology.c \
		doca/workload_grant.c \
		$(DOCA_LIBS) $(TLS_LIBS) $(CRYPTO_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/worker_mpsc_queue_test: tests/worker_mpsc_queue_test.c doca/object.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/worker_mpsc_queue_test.c -lpthread

$(TESTDIR)/topology_test: tests/topology_test.c include/dpumesh/dmesh_topology.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/topology_test.c

$(TESTDIR)/peer_channel_test: tests/peer_channel_test.c doca/peer_channel.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ tests/peer_channel_test.c doca/peer_channel.c \
		$(CRYPTO_LIBS) $(RPATHS)

$(TESTDIR)/peer_tls_test: tests/peer_tls_test.c doca/peer_tls.c doca/peer_tls.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/peer_tls_test.c doca/peer_tls.c \
		$(TLS_LIBS) $(CRYPTO_LIBS) $(RPATHS)

$(TESTDIR)/peer_transport_test: tests/peer_transport_test.c doca/peer_transport.c \
		doca/peer_wire_tcp.c doca/peer_tls.c doca/peer_channel.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ tests/peer_transport_test.c \
		doca/peer_transport.c doca/peer_wire_tcp.c doca/peer_tls.c doca/peer_channel.c \
		$(TLS_LIBS) $(CRYPTO_LIBS) $(RPATHS)

$(TESTDIR)/peer_wire_test: tests/peer_wire_test.c doca/peer_wire_tcp.c \
		doca/peer_wire_rdma.c doca/peer_wire.h | dirs
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ tests/peer_wire_test.c \
		doca/peer_wire_tcp.c doca/peer_wire_rdma.c $(RDMA_LIBS) $(RPATHS)

$(TESTDIR)/topology_gen_test: tests/topology_gen_test.c doca/topology.c doca/workload_grant.c doca/control_scope.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -o $@ tests/topology_gen_test.c doca/topology.c doca/workload_grant.c \
		doca/control_scope.c \
		$(DOCA_LIBS) $(CRYPTO_LIBS) $(RPATHS)

$(TESTDIR)/ring_counter_test: tests/ring_counter_test.c doca/ring.h doca/dpa_common.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/ring_counter_test.c -lpthread

$(TESTDIR)/l7_abi_contract_test: tests/l7_abi_contract_test.c linkerd/include/dmesh_l7.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/l7_abi_contract_test.c

$(TESTDIR)/benchmark_result_contract_test: tests/benchmark_result_contract_test.c \
		bench/apps/bench_result.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/benchmark_result_contract_test.c

# Tests that build and run without the DOCA SDK or a BlueField device.
# Kept separate so CI on a plain runner can verify them, and so "runs without
# hardware" is an explicit contract instead of tribal knowledge.
HOSTFREE_TESTS := $(TESTDIR)/topology_test $(TESTDIR)/l7_abi_contract_test \
	$(TESTDIR)/l4_pin_policy_test $(TESTDIR)/preload_api_contract_test \
	$(TESTDIR)/benchmark_result_contract_test $(TESTDIR)/peer_tls_test

test-hostfree: $(HOSTFREE_TESTS)
	@case "$(CFLAGS)" in *-DNDEBUG*) \
		echo "test-hostfree: NDEBUG disables assert(); these tests would silently pass" >&2; \
		exit 1;; esac
	$(TESTDIR)/topology_test
	$(TESTDIR)/l7_abi_contract_test
	$(TESTDIR)/l4_pin_policy_test
	$(TESTDIR)/preload_api_contract_test
	$(TESTDIR)/benchmark_result_contract_test
	$(TESTDIR)/peer_tls_test
	sh tests/dma_fault_scope_test.sh
	python3 tests/analyze_saturation_test.py
	python3 tests/workload_attest_agent_test.py
	python3 tests/dpumesh_controller_test.py
	python3 tests/dpumesh_webhook_test.py
	python3 tests/linkerd_cp_relay_test.py
	python3 tests/feed_delivery_test.py
	python3 tests/health_page_test.py
	bash tests/policy_route_judge_test.sh

test: $(TESTDIR)/native_api_contract_test $(TESTDIR)/native_control_state_test \
	$(TESTDIR)/workload_grant_test $(TESTDIR)/pod_membership_test \
	$(TESTDIR)/native_tx_batch_policy_test $(TESTDIR)/native_writable_test \
	$(TESTDIR)/preload_api_contract_test $(TESTDIR)/l4_pin_policy_test \
	$(TESTDIR)/lb_policy_test \
	$(TESTDIR)/proxy_lane_queue_test $(TESTDIR)/worker_mpsc_queue_test \
	$(TESTDIR)/topology_test $(TESTDIR)/topology_gen_test $(TESTDIR)/peer_channel_test \
	$(TESTDIR)/peer_tls_test $(TESTDIR)/peer_transport_test \
	$(TESTDIR)/peer_wire_test $(TESTDIR)/ring_counter_test \
	$(TESTDIR)/l7_abi_contract_test $(TESTDIR)/benchmark_result_contract_test $(PRELOAD) \
	$(BINDIR)/bench_dpumesh $(BINDIR)/bench_sock
	$(TESTDIR)/native_api_contract_test
	$(TESTDIR)/native_control_state_test
	$(TESTDIR)/workload_grant_test
	$(TESTDIR)/pod_membership_test
	$(TESTDIR)/native_tx_batch_policy_test
	$(TESTDIR)/native_writable_test
	$(TESTDIR)/preload_api_contract_test
	$(TESTDIR)/l4_pin_policy_test
	$(TESTDIR)/lb_policy_test
	$(TESTDIR)/proxy_lane_queue_test
	$(TESTDIR)/worker_mpsc_queue_test
	$(TESTDIR)/topology_test
	$(TESTDIR)/topology_gen_test
	$(TESTDIR)/peer_channel_test
	$(TESTDIR)/peer_tls_test
	$(TESTDIR)/peer_transport_test
	$(TESTDIR)/peer_wire_test
	$(TESTDIR)/ring_counter_test
	$(TESTDIR)/l7_abi_contract_test
	$(TESTDIR)/benchmark_result_contract_test
	sh tests/dma_fault_scope_test.sh
	sh tests/abi_contract_test.sh $(LIB) $(PRELOAD) $(ABI_MAJOR)
	sh tests/generator_selftest_test.sh $(BINDIR)/bench_dpumesh $(BINDIR)/bench_sock
	python3 tests/analyze_saturation_test.py
	python3 tests/workload_attest_agent_test.py
	python3 tests/dpumesh_controller_test.py
	python3 tests/dpumesh_webhook_test.py
	python3 tests/linkerd_cp_relay_test.py
	python3 tests/feed_delivery_test.py
	python3 tests/health_page_test.py
	bash tests/policy_route_judge_test.sh

# dmesh API binaries link the transport library. One explicit rule each so the
# source is a tracked prerequisite (rebuilds on edit).
define DMESH_BIN_RULE
$(BINDIR)/$(1): $($(1)_SRC) $(LIB_LINK) | dirs
	$$(CC) -O2 -g $$(DEPFLAGS) -Iinclude -Isrc -o $$@ $$< -L$$(LIBDIR) -ldpumesh -lpthread $$($(1)_LIBS) $$(RPATHS)
	@echo "  -> $$@"
endef
$(foreach b,$(DMESH_BINS),$(eval $(call DMESH_BIN_RULE,$(b))))

# The shim's data/EQ plane is a client of the public native API. It also compiles
# against src/dmesh_core.h for narrow in-tree address-resolution and FIN hooks.
$(PRELOAD): src/dmesh_preload.c $(LIB_LINK) | dirs
	$(CC) -O2 -g $(DEPFLAGS) -fPIC -shared -Iinclude -Isrc -o $@ src/dmesh_preload.c \
		-L$(LIBDIR) -ldpumesh -lpthread -ldl $(RPATHS)
	@echo "  -> $@"

# pure-POSIX validators (no library dependency); tcp_client needs pthread
$(BINDIR)/tcp_echo: bench/validators/tcp_echo.c | dirs
	$(CC) -O2 $(DEPFLAGS) -o $@ $<
$(BINDIR)/tcp_client: bench/validators/tcp_client.c | dirs
	$(CC) -O2 $(DEPFLAGS) -o $@ $< -lpthread
$(BINDIR)/preload_runner: bench/validators/preload_runner.c | dirs
	$(CC) -O2 $(DEPFLAGS) -o $@ $<
# matched C TCP baseline: -Ibench/apps for bench.h; bench_sock uses libm (Poisson arrivals)
$(BINDIR)/echo_sock: bench/apps/echo_sock.c | dirs
	$(CC) -O2 -g $(DEPFLAGS) -Ibench/apps -o $@ $< -lpthread
$(BINDIR)/bench_sock: bench/apps/bench_sock.c | dirs
	$(CC) -O2 -g $(DEPFLAGS) -Ibench/apps -o $@ $< -lpthread -lm
# HTTP/1.1 pair: the only workload in the tree that drives the protocol-aware
# path's HTTP/1 branch.
$(BINDIR)/http1_echo: bench/apps/http1_echo.c | dirs
	$(CC) -O2 -g $(DEPFLAGS) -Ibench/apps -o $@ $< -lpthread
$(BINDIR)/http1_bench: bench/apps/http1_bench.c | dirs
	$(CC) -O2 -g $(DEPFLAGS) -Ibench/apps -o $@ $< -lpthread

clean:
	rm -rf $(BUILD)

-include $(wildcard $(DEPDIR)/*.d)
