# DPUmesh — standalone host build (no Thrift, no CMake).
#
# Produces the host transport library + the benchmark/validator binaries.
# The DPU-side control plane (ARM + DPA kernel) is built separately with meson
# under doca/ (see doca/meson.build); test-bench.sh drives that on the DPU.
#
#   make            # libdpumesh.so + all bench binaries (+ Go binaries if `go` present)
#   make lib        # build/lib/libdpumesh.so only
#   make bench      # C bench/validator binaries + the LD_PRELOAD shim
#   make go         # Go TCP-baseline binaries (bench_tcp, echo_tcp)
#   make clean

CC      ?= gcc
DOCA_PKGS := doca-common doca-comch doca-dpa
DOCA_CFLAGS := $(shell pkg-config --cflags $(DOCA_PKGS))
DOCA_LIBS   := $(shell pkg-config --libs   $(DOCA_PKGS))
DOCA_LIBDIR := $(shell pkg-config --variable=libdir doca-common)
FLEXIO_LIBDIR := /opt/mellanox/flexio/lib

BUILD   := build
LIBDIR  := $(BUILD)/lib
BINDIR  := $(BUILD)/bin

# -Iinclude → <dpumesh/...> ; -I. → the "doca/..." includes from src/dpumesh_doca.c
CFLAGS  := -O2 -g -Wall -fPIC -DDOCA_ALLOW_EXPERIMENTAL_API -Iinclude -I. $(DOCA_CFLAGS)

# Runtime search paths. In a container everything is copied to /usr/local/lib;
# for a local build we also point at the in-tree lib dir and the DOCA libs.
RPATHS  := -Wl,-rpath,/usr/local/lib \
           -Wl,-rpath,$(abspath $(LIBDIR)) \
           -Wl,-rpath,$(DOCA_LIBDIR) \
           -Wl,-rpath,$(FLEXIO_LIBDIR)

# ---- host transport library --------------------------------------------------
LIB_SRCS := \
	src/dpumesh_doca.c \
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

LIB := $(LIBDIR)/libdpumesh.so

# ---- consumers of the library ------------------------------------------------
# dmesh_* API clients (socket/epoll façade over dpm.h)
DMESH_BINS := bench_dpumesh echo_dpumesh loopback_dpumesh stream_dpumesh
bench_dpumesh_SRC    := bench/bench_sock.c
echo_dpumesh_SRC     := bench/echo_sock.c
loopback_dpumesh_SRC := bench/loopback_sock.c
stream_dpumesh_SRC   := bench/stream_sock.c

# LD_PRELOAD shim (interposes libc sockets → dmesh) + its vanilla-TCP validators
PRELOAD := $(LIBDIR)/libdmesh_preload.so
PLAIN_BINS := tcp_echo tcp_client preload_runner        # pure POSIX, no dmesh link
tcp_echo_SRC       := bench/tcp_echo.c
tcp_client_SRC     := bench/tcp_client.c
preload_runner_SRC := bench/preload_runner.c

.PHONY: all lib bench go clean dirs
all: lib bench go

dirs:
	@mkdir -p $(LIBDIR) $(BINDIR)

lib: dirs $(LIB)
$(LIB): $(LIB_SRCS)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libdpumesh.so -o $@ $(LIB_SRCS) \
		$(DOCA_LIBS) -lpthread $(RPATHS)
	@echo "  -> $@"

bench: lib $(addprefix $(BINDIR)/,$(DMESH_BINS)) $(PRELOAD) $(addprefix $(BINDIR)/,$(PLAIN_BINS))

# dmesh API binaries link the transport library. One explicit rule each so the
# source is a tracked prerequisite (rebuilds on edit).
define DMESH_BIN_RULE
$(BINDIR)/$(1): $($(1)_SRC) | dirs lib
	$$(CC) -O2 -g -Iinclude -o $$@ $$< -L$$(LIBDIR) -ldpumesh -lpthread $$(RPATHS)
	@echo "  -> $$@"
endef
$(foreach b,$(DMESH_BINS),$(eval $(call DMESH_BIN_RULE,$(b))))

# the shim itself needs the dmesh_* symbols → links the library too
$(PRELOAD): src/dmesh_preload.c | dirs lib
	$(CC) -O2 -g -fPIC -shared -Iinclude -o $@ src/dmesh_preload.c \
		-L$(LIBDIR) -ldpumesh -lpthread -ldl $(RPATHS)
	@echo "  -> $@"

# pure-POSIX validators (no library dependency); tcp_client needs pthread
$(BINDIR)/tcp_echo: bench/tcp_echo.c | dirs
	$(CC) -O2 -o $@ $<
$(BINDIR)/tcp_client: bench/tcp_client.c | dirs
	$(CC) -O2 -o $@ $< -lpthread
$(BINDIR)/preload_runner: bench/preload_runner.c | dirs
	$(CC) -O2 -o $@ $<

# Go TCP baseline (skipped with a notice if `go` is absent)
go: dirs
	@command -v go >/dev/null 2>&1 || { echo "  (go not found — skipping bench_tcp/echo_tcp)"; exit 0; }
	cd bench && go build -o ../$(BINDIR)/bench_tcp bench_tcp.go && go build -o ../$(BINDIR)/echo_tcp echo_tcp.go
	@echo "  -> $(BINDIR)/bench_tcp $(BINDIR)/echo_tcp"

clean:
	rm -rf $(BUILD)
