# Build the host transport library and benchmark/validator binaries. DPU ARM and
# DPA components use doca/meson.build. Targets: all, lib, bench, go, clean.

CC      ?= gcc
DOCA_PKGS := doca-common doca-comch doca-dpa
DOCA_CFLAGS := $(shell pkg-config --cflags $(DOCA_PKGS))
DOCA_LIBS   := $(shell pkg-config --libs   $(DOCA_PKGS))
DOCA_LIBDIR := $(shell pkg-config --variable=libdir doca-common)
FLEXIO_LIBDIR := /opt/mellanox/flexio/lib

BUILD   := build
LIBDIR  := $(BUILD)/lib
BINDIR  := $(BUILD)/bin
TESTDIR := $(BUILD)/test

# -Iinclude → <dpumesh/...> ; -I. → the "doca/..." includes from src/dmesh_core.c
CFLAGS  := -O2 -g -Wall -fPIC -DDOCA_ALLOW_EXPERIMENTAL_API -Iinclude -I. $(DOCA_CFLAGS)

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
	src/dmesh_resolve.c \
	doca/common.c \
	doca/object.c \
	doca/buffer.c \
	doca/ring.c \
	doca/comch_common.c \
	doca/comch_client.c \
	doca/comch_server.c \
	doca/comch_consumer.c \
	doca/comch_msgq.c \
	doca/dpa.c
LIB_HDRS := $(shell rg --files include src doca -g '*.h')

# ABI major. BUMP IT whenever the public ABI changes incompatibly — a field added to
# dmesh_event_t / dmesh_qp_t / dmesh_channel_t, a reorder, a signature change. The SONAME
# identifies incompatible public layouts at load time.
ABI_MAJOR := 4
LIB      := $(LIBDIR)/libdpumesh.so.$(ABI_MAJOR)
LIB_LINK := $(LIBDIR)/libdpumesh.so

# ---- consumers of the library ------------------------------------------------
# dmesh_* API clients (socket/epoll façade over dmesh.h)
DMESH_BINS := bench_dpumesh echo_dpumesh loopback_dpumesh stream_dpumesh verbs_dpumesh
bench_dpumesh_SRC    := bench/apps/bench_dpumesh.c
echo_dpumesh_SRC     := bench/apps/echo_dpumesh.c
loopback_dpumesh_SRC := bench/validators/loopback_dpumesh.c
stream_dpumesh_SRC   := bench/validators/stream_dpumesh.c
verbs_dpumesh_SRC    := bench/validators/verbs_dpumesh.c

# LD_PRELOAD shim (interposes libc sockets → dmesh) + its vanilla-TCP validators
PRELOAD := $(LIBDIR)/libdmesh_preload.so
PLAIN_BINS := tcp_echo tcp_client preload_runner bench_sock echo_sock  # pure POSIX, no dmesh link
tcp_echo_SRC       := bench/validators/tcp_echo.c
tcp_client_SRC     := bench/validators/tcp_client.c
preload_runner_SRC := bench/validators/preload_runner.c
# bench_sock/echo_sock: the MATCHED C-language TCP baseline (bench.h wire frame). Same
# binary runs over direct TCP, TCP+Envoy, and DPUmesh-preload — isolating the transport.
bench_sock_SRC     := bench/apps/bench_sock.c
echo_sock_SRC      := bench/apps/echo_sock.c

.PHONY: all lib bench test clean dirs
all: lib bench

# Header dependencies for the library and all consumers.
DEPDIR  := $(BUILD)/dep
DEPFLAGS = -MMD -MP -MF $(DEPDIR)/$(@F).d

dirs:
	@mkdir -p $(LIBDIR) $(BINDIR) $(DEPDIR) $(TESTDIR)

lib: dirs $(LIB) $(LIB_LINK)
$(LIB): $(LIB_SRCS) $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -shared -Wl,-soname,libdpumesh.so.$(ABI_MAJOR) -o $@ $(LIB_SRCS) \
		$(DOCA_LIBS) -lpthread $(RPATHS)
	@echo "  -> $@"

# The unversioned name is the LINKER's entry point only (-ldpumesh). What a binary
# records as DT_NEEDED is the SONAME above, so runtime resolution never goes through it.
$(LIB_LINK): $(LIB)
	@ln -sf $(notdir $(LIB)) $@

bench: lib $(addprefix $(BINDIR)/,$(DMESH_BINS)) $(PRELOAD) $(addprefix $(BINDIR)/,$(PLAIN_BINS))

# Focused host-only contract tests. Function-section GC lets each test link the
# production source that owns the state machine without constructing DOCA hardware.
$(TESTDIR)/native_api_contract_test: tests/native_api_contract_test.c src/dmesh_api.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/native_api_contract_test.c src/dmesh_api.c

$(TESTDIR)/native_control_state_test: tests/native_control_state_test.c doca/comch_server.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/native_control_state_test.c doca/comch_server.c $(DOCA_LIBS) -lpthread $(RPATHS)

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

$(TESTDIR)/proxy_lane_queue_test: tests/proxy_lane_queue_test.c doca/dpu_proxy.c $(LIB_HDRS) | dirs
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ tests/proxy_lane_queue_test.c $(DOCA_LIBS) -lpthread $(RPATHS)

$(TESTDIR)/ingest_mpsc_queue_test: tests/ingest_mpsc_queue_test.c doca/object.h | dirs
	$(CC) $(CFLAGS) -o $@ tests/ingest_mpsc_queue_test.c -lpthread

test: $(TESTDIR)/native_api_contract_test $(TESTDIR)/native_control_state_test \
	$(TESTDIR)/native_tx_batch_policy_test $(TESTDIR)/native_writable_test \
	$(TESTDIR)/preload_api_contract_test $(TESTDIR)/l4_pin_policy_test \
	$(TESTDIR)/proxy_lane_queue_test $(TESTDIR)/ingest_mpsc_queue_test $(PRELOAD)
	$(TESTDIR)/native_api_contract_test
	$(TESTDIR)/native_control_state_test
	$(TESTDIR)/native_tx_batch_policy_test
	$(TESTDIR)/native_writable_test
	$(TESTDIR)/preload_api_contract_test
	$(TESTDIR)/l4_pin_policy_test
	$(TESTDIR)/proxy_lane_queue_test
	$(TESTDIR)/ingest_mpsc_queue_test
	sh tests/abi_contract_test.sh $(LIB) $(PRELOAD) $(ABI_MAJOR)

# dmesh API binaries link the transport library. One explicit rule each so the
# source is a tracked prerequisite (rebuilds on edit).
define DMESH_BIN_RULE
$(BINDIR)/$(1): $($(1)_SRC) $(LIB_LINK) | dirs
	$$(CC) -O2 -g $$(DEPFLAGS) -Iinclude -Isrc -o $$@ $$< -L$$(LIBDIR) -ldpumesh -lpthread $$(RPATHS)
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

clean:
	rm -rf $(BUILD)

-include $(wildcard $(DEPDIR)/*.d)
