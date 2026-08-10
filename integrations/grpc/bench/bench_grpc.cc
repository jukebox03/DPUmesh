/* gRPC client for the shared benchmark harness. One issuer owns the arrival
 * timeline; one completer per channel records latency from intended arrival. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <thread>
#include <vector>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include "dmesh_api_ops.h"
#include "dmesh_grpc_runtime.h"
#include "dmesh_runtime.h"
#include "dmesh_reactor.h"
#include "src/proto/grpc/testing/benchmark_service.grpc.pb.h"

/* Shared frame and latency histogram. */
#include "bench.h"

namespace {

using dpumesh::grpc::DmeshRuntime;
using ::grpc::testing::BenchmarkService;
using ::grpc::testing::SimpleRequest;
using ::grpc::testing::SimpleResponse;

constexpr int kCtrlPortDefault = 9092;
/* Maximum outstanding calls per channel. */
constexpr size_t kInflightRing = 1u << 16;
constexpr long kOpenCap = static_cast<long>(kInflightRing / 2);
constexpr int kModeClosed = 0;
constexpr int kModeOpen = 1;
constexpr int kArrConst = 0;
constexpr int kArrPoisson = 1;
constexpr int kMaxThreads = 64;

std::string g_transport = "dmesh";
std::string g_service = "echo-grpc-dpumesh";
std::string g_host = "127.0.0.1";
int g_port = 9091;
int g_reactors = 8;
std::shared_ptr<DmeshRuntime> g_runtime;

struct Worker;

/* ------------------------------------------------------------ one in-flight RPC */
struct Call {
  ::grpc::ClientContext context;
  SimpleResponse response;
  ::grpc::Status status;
  std::unique_ptr<::grpc::ClientAsyncResponseReader<SimpleResponse>> reader;
  double scheduled = 0.0;   /* intended arrival, not send time */
  Worker* worker = nullptr;
};

struct Worker {
  std::shared_ptr<::grpc::Channel> channel;
  std::unique_ptr<BenchmarkService::Stub> stub;
  ::grpc::CompletionQueue cq;

  /* Issued calls retained until completion or cancellation. */
  std::mutex live_mu;
  std::unordered_set<Call*> live;
  std::atomic<long> outstanding{0};
  std::atomic<long> issued{0};

  /* Completer-owned. */
  bench_hist_t hist{};
  long rcnt = 0, fail = 0;
  double warmup_end = 0.0, end = 0.0, dura = 0.0;

  int mode = kModeOpen;
  int window = 1;
  long warmup = 0;
  double measure_end = 0.0;
  SimpleRequest request;
  std::atomic<int> broken{0};
};

std::atomic<int> g_stop{0};


/* ------------------------------------------------------------ channels */
std::shared_ptr<::grpc::Channel> MakeChannel(int index) {
  ::grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
  /* One transport connection per worker. */
  args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
  args.SetInt("bench.channel_index", index);

  if (g_transport == "dmesh") {
    auto channel = dpumesh::grpc::CreateDmeshChannel(
        g_runtime, g_service, ::grpc::InsecureChannelCredentials(), args);
    if (!channel.ok()) {
      std::fprintf(stderr, "[bench_grpc] channel %d failed: %s\n", index,
                   channel.status().ToString().c_str());
      return nullptr;
    }
    return *channel;
  }
  char target[256];
  std::snprintf(target, sizeof target, "%s:%d", g_host.c_str(), g_port);
  return ::grpc::CreateCustomChannel(
      target, ::grpc::InsecureChannelCredentials(), args);
}

/* ------------------------------------------------------------ issue */
void Issue(Worker* w, double scheduled) {
  auto* call = new Call();
  call->scheduled = scheduled;
  call->worker = w;
  {
    std::lock_guard<std::mutex> lock(w->live_mu);
    w->live.insert(call);
  }
  w->outstanding.fetch_add(1, std::memory_order_acq_rel);
  w->issued.fetch_add(1, std::memory_order_relaxed);
  call->reader = w->stub->AsyncUnaryCall(&call->context, w->request, &w->cq);
  call->reader->Finish(&call->response, &call->status, call);
}

/* ------------------------------------------------------------ completer */
void CompleterMain(Worker* w) {
  if (bench_hist_init(&w->hist) < 0) { w->broken.store(1); return; }

  if (w->mode == kModeClosed) {
    for (int i = 0; i < w->window; ++i) Issue(w, bench_now_sec());
  }

  for (;;) {
    void* tag = nullptr;
    bool ok = false;
    if (!w->cq.Next(&tag, &ok)) break;      /* shut down and drained */

    std::unique_ptr<Call> call(static_cast<Call*>(tag));
    {
      std::lock_guard<std::mutex> lock(w->live_mu);
      w->live.erase(call.get());
    }
    w->outstanding.fetch_sub(1, std::memory_order_acq_rel);

    const double now = bench_now_sec();
    if (now > w->measure_end) continue;     /* outside the timed window */

    if (!ok || !call->status.ok()) {
      ++w->fail;
    } else {
      if (w->rcnt >= w->warmup) {
        bench_hist_record(&w->hist, (now - call->scheduled) * 1e6);
      }
      ++w->rcnt;
      if (w->rcnt == w->warmup) w->warmup_end = now;
    }
    /* Closed loop refills its own window: one completion frees one slot. */
    if (w->mode == kModeClosed && now <= w->measure_end && !g_stop.load()) {
      Issue(w, bench_now_sec());
    }
  }
}

/* ------------------------------------------------------------ issuer */
/* Each issuer draws its own gaps, so the generator state is per thread. */
thread_local uint64_t g_prng = 0x9e3779b97f4a7c15ull;

double PrngExpGap(double rate) {
  g_prng ^= g_prng << 13; g_prng ^= g_prng >> 7; g_prng ^= g_prng << 17;
  const double u = (static_cast<double>(g_prng >> 11) + 1.0) /
                   9007199254740993.0;
  return -std::log(u) / rate;
}

void SleepUntil(double deadline) {
  double left = deadline - bench_now_sec();
  if (left <= 0.0) return;
  if (left > 0.020) left = 0.020;
  timespec delay{static_cast<time_t>(left),
                 static_cast<long>((left - static_cast<double>(
                                               static_cast<time_t>(left))) * 1e9)};
  nanosleep(&delay, nullptr);
}

/* One arrival timeline per issuer. Issuer `index` owns the workers at
   index, index + count, ... and its own 1/count share of the rate, and starts
   index/rate into the first interval, so the merged timeline keeps the
   requested spacing. Issuing from a single thread caps the offered rate at
   whatever one core can push through `Issue`, which is a property of the
   generator rather than of the transport under test. */
void IssuerMain(std::vector<std::unique_ptr<Worker>>* workers, double rate,
                int arrival, double start_at, double duration,
                std::atomic<long>* drops, size_t index, size_t count) {
  while (bench_now_sec() < start_at) {
    if (g_stop.load()) return;
    SleepUntil(start_at);
  }
  g_prng ^= 0x9e3779b97f4a7c15ull * (index + 1);
  if (g_prng == 0) g_prng = 0x9e3779b97f4a7c15ull;

  const double start = start_at;
  const double share = rate / static_cast<double>(count);
  double sched_next = start + static_cast<double>(index) / rate;
  size_t rr = index;
  const size_t n = workers->size();

  for (;;) {
    double now = bench_now_sec();
    if (now - start > duration || g_stop.load()) break;
    while (now >= sched_next) {
      const double scheduled = sched_next;
      sched_next += (arrival == kArrPoisson) ? PrngExpGap(share) : 1.0 / share;
      Worker* w = (*workers)[rr % n].get();
      rr += count;
      if (w->outstanding.load(std::memory_order_acquire) >= kOpenCap) {
        drops->fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      Issue(w, scheduled);
    }
    SleepUntil(sched_next);
  }
}

/* ------------------------------------------------------------ generator selftest */
int SelfTest(char* reply, size_t reply_size, int payload, int threads,
             double duration, double rate, int arrival) {
  if (reply == nullptr || reply_size == 0 || payload < 0 || threads < 1 ||
      !std::isfinite(duration) || !std::isfinite(rate) || duration <= 0.0 ||
      rate <= 0.0 || (arrival != 0 && arrival != 1)) {
    return -1;
  }

  std::vector<uint8_t> frame(BENCH_HDR_LEN + static_cast<size_t>(payload));
  uint64_t scheduled = 0, drops = 0, checksum = 0;
  g_prng = 0x9e3779b97f4a7c15ull;

  const double start = bench_now_sec();
  const double stop_at = start + duration;
  double sched_next = start;
  for (;;) {
    const double now = bench_now_sec();
    if (now > stop_at) break;
    while (now >= sched_next) {
      /* Same per-arrival work the issuer does before handing off to gRPC. */
      bench_put_hdr(frame.data(), BENCH_REQ_MAGIC,
                    static_cast<uint32_t>(scheduled),
                    static_cast<uint32_t>(payload),
                    static_cast<uint32_t>(payload));
      std::memset(frame.data() + BENCH_HDR_LEN, 42,
                  static_cast<size_t>(payload));
      __asm__ __volatile__("" : : "r"(frame.data()) : "memory");
      checksum ^= frame[scheduled % frame.size()];
      ++scheduled;
      sched_next += (arrival == kArrPoisson) ? PrngExpGap(rate) : 1.0 / rate;
    }
    SleepUntil(sched_next);
  }
  const double elapsed = bench_now_sec() - start;

  if (arrival == kArrConst) {
    const uint64_t target =
        static_cast<uint64_t>(std::ceil(rate * duration - 1e-9));
    if (target > scheduled) drops = target - scheduled;
  } else {
    while (sched_next < stop_at) { ++drops; sched_next += PrngExpGap(rate); }
  }

  const double expected = rate * duration;
  const double schedule_ratio =
      expected > 0.0 ? static_cast<double>(scheduled) / expected : 0.0;
  const double drop_ratio =
      (scheduled + drops) ? static_cast<double>(drops) /
                                static_cast<double>(scheduled + drops)
                          : 0.0;
  std::snprintf(reply, reply_size,
                "OK selftest=1 frame=%u payload=%d threads=%d durs=%.6f "
                "elapsed=%.6f offered_rps=%.3f scheduled=%llu drops=%llu "
                "schedule_ratio=%.6f drop_ratio=%.6f checksum=%llu arr=%s\n",
                BENCH_HDR_LEN + static_cast<uint32_t>(payload), payload,
                threads, duration, elapsed, rate,
                static_cast<unsigned long long>(scheduled),
                static_cast<unsigned long long>(drops), schedule_ratio,
                drop_ratio, static_cast<unsigned long long>(checksum),
                arrival ? "poisson" : "const");
  return 0;
}

/* ------------------------------------------------------------ one run */
void RunBench(int conn_fd, int mode, int req_size, int reply_size, int window,
              double duration, long warmup, int threads, double rate,
              int arrival, int channels) {
  char reply[2048];

  if (req_size < 0 || reply_size < 1 || duration <= 0 || threads < 1 ||
      (mode == kModeOpen && rate <= 0) ||
      (mode == kModeClosed && window < 1)) {
    std::snprintf(reply, sizeof reply, "ERR invalid args\n");
    if (write(conn_fd, reply, std::strlen(reply)) < 0) {}
    return;
  }
  if (threads > kMaxThreads) threads = kMaxThreads;
  if (warmup < 0) warmup = 0;
  /* Workers share the channel pool round-robin, so the load the client offers
   * is held fixed while the transport connection count varies. */
  if (channels < 1 || channels > threads) channels = threads;

  std::fprintf(stderr,
               "[bench_grpc] %s req=%d reply=%d dur=%.1fs warmup=%ld conns=%d "
               "channels=%d transport=%s target=%s\n",
               mode == kModeOpen ? "OPEN" : "RUN", req_size, reply_size,
               duration, warmup, threads, channels, g_transport.c_str(),
               g_transport == "dmesh" ? g_service.c_str() : g_host.c_str());

  std::vector<std::shared_ptr<::grpc::Channel>> channel_pool;
  channel_pool.reserve(channels);
  for (int i = 0; i < channels; ++i) {
    auto channel = MakeChannel(i);
    if (channel == nullptr ||
        !channel->WaitForConnected(std::chrono::system_clock::now() +
                                   std::chrono::seconds(20))) {
      std::snprintf(reply, sizeof reply, "ERR connect failed on channel %d\n", i);
      if (write(conn_fd, reply, std::strlen(reply)) < 0) {}
      return;
    }
    channel_pool.push_back(std::move(channel));
  }

  std::vector<std::unique_ptr<Worker>> workers;
  workers.reserve(threads);
  for (int i = 0; i < threads; ++i) {
    auto w = std::make_unique<Worker>();
    w->channel = channel_pool[i % channels];
    w->stub = BenchmarkService::NewStub(w->channel);
    w->mode = mode;
    w->window = window;
    w->warmup = warmup;
    w->request.set_response_type(::grpc::testing::COMPRESSABLE);
    w->request.set_response_size(reply_size);
    w->request.mutable_payload()->set_type(::grpc::testing::COMPRESSABLE);
    w->request.mutable_payload()->set_body(std::string(req_size, '\0'));
    workers.push_back(std::move(w));
  }

  const double start_at = bench_now_sec() + 0.3;
  const double measure_end = start_at + duration;
  for (auto& w : workers) {
    w->measure_end = measure_end;
    w->warmup_end = start_at;
  }

  g_stop.store(0);

  /* Transport counters over the measured window. */
  dmesh_tx_stats_t tx0{}, tx1{};
  dpumesh::grpc::DmeshReactor::Stats st0{}, st1{};
  if (g_runtime != nullptr) {
    st0 = g_runtime->stats();
    dmesh_get_tx_stats(g_runtime->channel(), &tx0);
  }

  std::atomic<long> drops{0};

  std::vector<std::thread> completers;
  completers.reserve(threads);
  for (auto& w : workers) completers.emplace_back([&w] { CompleterMain(w.get()); });

  std::vector<std::thread> issuers;
  if (mode == kModeOpen) {
    const size_t n_issuers = workers.size();
    issuers.reserve(n_issuers);
    for (size_t i = 0; i < n_issuers; ++i) {
      issuers.emplace_back([&, i] {
        IssuerMain(&workers, rate, arrival, start_at, duration, &drops,
                   i, n_issuers);
      });
    }
  } else {
    while (bench_now_sec() < start_at && !g_stop.load()) SleepUntil(start_at);
  }

  /* The measurement window is wall-clock, identical for every worker. */
  while (bench_now_sec() < measure_end && !g_stop.load()) SleepUntil(measure_end);
  const double end = bench_now_sec();
  if (g_runtime != nullptr) {
    st1 = g_runtime->stats();
    dmesh_get_tx_stats(g_runtime->channel(), &tx1);
  }
  g_stop.store(1);
  for (auto& t : issuers) {
    if (t.joinable()) t.join();
  }

  for (auto& w : workers) {
    w->end = end;
    /* Cancel in-flight calls while holding their ownership lock. */
    {
      std::lock_guard<std::mutex> lock(w->live_mu);
      for (Call* call : w->live) call->context.TryCancel();
    }
    w->cq.Shutdown();
  }
  for (auto& t : completers) t.join();

  bench_hist_t agg{};
  bench_hist_init(&agg);
  double mrps = 0.0, request_gbps = 0.0, response_gbps = 0.0;
  const double request_frame = BENCH_HDR_LEN + static_cast<double>(req_size);
  const double response_frame = BENCH_HDR_LEN + static_cast<double>(reply_size);
  long total_ok = 0, total_fail = 0;
  long total_scheduled = 0, total_pending = 0;
  int worker_fail = 0;

  for (auto& w : workers) {
    long measured = w->rcnt - w->warmup;
    if (measured < 0) measured = 0;
    w->dura = (w->rcnt > w->warmup && w->end > w->warmup_end)
                  ? w->end - w->warmup_end
                  : 0.0;
    total_fail += w->fail;
    if (w->broken.load()) { ++worker_fail; ++total_fail; }
    total_scheduled += w->issued.load(std::memory_order_relaxed);
    total_pending += w->outstanding.load(std::memory_order_relaxed);
    if (!w->broken.load() && w->dura > 1e-9 && measured > 0) {
      const double rps = static_cast<double>(measured) / w->dura;
      mrps += rps * 1e-6;
      request_gbps += 8e-9 * rps * request_frame;
      response_gbps += 8e-9 * rps * response_frame;
      total_ok += measured;
      bench_hist_merge(&agg, &w->hist);
    }
    bench_hist_free(&w->hist);
  }
  total_scheduled += drops.load(std::memory_order_relaxed);

  const double p50 = bench_hist_pct(&agg, 50.0);
  const double p95 = bench_hist_pct(&agg, 95.0);
  const double p99 = bench_hist_pct(&agg, 99.0);
  const double p999 = bench_hist_pct(&agg, 99.9);
  const double p9999 = bench_hist_pct(&agg, 99.99);
  const double avg = bench_hist_avg(&agg);
  const double mn = bench_hist_min(&agg);
  const double mx = bench_hist_max(&agg);
  const unsigned long long overflow = agg.overflow;
  bench_hist_free(&agg);

  const double offered_mrps = (mode == kModeOpen) ? rate * 1e-6 : mrps;
  const double gbps = request_gbps + response_gbps;

  std::snprintf(
      reply, sizeof reply,
      "OK mrps=%.6f gbps=%.4f req_gbps=%.4f resp_gbps=%.4f "
      "p50=%.2f p95=%.2f p99=%.2f p999=%.2f p9999=%.2f "
      "avg=%.2f min=%.2f max=%.2f rcnt=%ld scheduled=%ld pending=%ld fail=%ld "
      "conc=%d threads=%d channels=%d reqsz=%d repsz=%d reqframe=%u respframe=%u "
      "durs=%.3f offered_mrps=%.6f drops=%ld overflow=%llu worker_fail=%d "
      "reorder=0 mode=%s arr=%s batch=0 reconns=0 reconn_us=0.00 "
      "grabs=%llu rets=%llu recyc=%llu waits=%llu pads=%llu "
      "credit_hold_dropped=%llu eq_budget_exhausted=%llu dist=NA\n",
      mrps, gbps, request_gbps, response_gbps, p50, p95, p99, p999, p9999, avg,
      mn, mx, total_ok, total_scheduled, total_pending, total_fail, window,
      threads, channels, req_size, reply_size,
      BENCH_HDR_LEN + static_cast<uint32_t>(req_size),
      BENCH_HDR_LEN + static_cast<uint32_t>(reply_size), duration, offered_mrps,
      drops.load(std::memory_order_relaxed), overflow, worker_fail,
      mode == kModeOpen ? "open" : "closed",
      arrival == kArrPoisson ? "poisson" : "const",
      tx1.pool_grabs - tx0.pool_grabs, tx1.pool_returns - tx0.pool_returns,
      tx1.recycle_hits - tx0.recycle_hits, tx1.grow_waits - tx0.grow_waits,
      tx1.block_pads - tx0.block_pads,
      static_cast<unsigned long long>(st1.receive_credit_hold_dropped -
                                      st0.receive_credit_hold_dropped),
      static_cast<unsigned long long>(st1.eq_drain_budget_exhausted -
                                      st0.eq_drain_budget_exhausted));
  if (write(conn_fd, reply, std::strlen(reply)) < 0) {}
}

/* ------------------------------------------------------------ control */
int CtrlListen(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 ||
      ::listen(fd, 8) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void HandleControl(int fd) {
  char buf[512];
  const ssize_t n = ::read(fd, buf, sizeof buf - 1);
  if (n <= 0) { ::close(fd); return; }
  buf[n] = '\0';
  if (char* p = std::strchr(buf, '\n')) *p = '\0';
  if (char* p = std::strchr(buf, '\r')) *p = '\0';

  if (std::strncmp(buf, "PING", 4) == 0) {
    if (write(fd, "PONG\n", 5) < 0) {}
    ::close(fd);
    return;
  }

  char cmd[16] = {0};
  if (std::sscanf(buf, "%15s", cmd) == 1 && std::strcmp(cmd, "RUN") == 0) {
    int req = 32, rep = 8, conc = 1, threads = 1, batch = 0;
    double dur = 10.0;
    long warm = 1000, reconn = 0;
    std::sscanf(buf, "%*s %d %d %d %lf %ld %d %ld %d", &req, &rep, &conc, &dur,
                &warm, &threads, &reconn, &batch);
    RunBench(fd, kModeClosed, req, rep, conc, dur, warm, threads, 0.0, kArrConst,
             0);
    ::close(fd);
    return;
  }
  if (std::sscanf(buf, "%15s", cmd) == 1 && std::strcmp(cmd, "OPEN") == 0) {
    int req = 32, rep = 8, threads = 1;
    double dur = 10.0, rate = 100000.0;
    long warm = 1000;
    char arr[16] = "const";
    /* Omitted or non-positive: one channel per worker. */
    int channels = 0;
    std::sscanf(buf, "%*s %d %d %d %lf %ld %lf %15s %d", &req, &rep, &threads,
                &dur, &warm, &rate, arr, &channels);
    const int arrival = std::strcmp(arr, "poisson") == 0 ? kArrPoisson : kArrConst;
    RunBench(fd, kModeOpen, req, rep, 0, dur, warm, threads, rate, arrival,
             channels);
    ::close(fd);
    return;
  }
  if (std::sscanf(buf, "%15s", cmd) == 1 && std::strcmp(cmd, "SELFTEST") == 0) {
    int payload = 0, threads = 0;
    double duration = 0.0, rate = 0.0;
    char arr[16] = {0};
    char out[512];
    const int fields =
        std::sscanf(buf, "%*s %d %d %lf %lf %15s", &payload, &threads,
                    &duration, &rate, arr);
    const int arrival =
        fields == 5 && std::strcmp(arr, "poisson") == 0 ? 1 : 0;
    if (fields != 5 ||
        (std::strcmp(arr, "const") != 0 && std::strcmp(arr, "poisson") != 0) ||
        threads > kMaxThreads ||
        SelfTest(out, sizeof out, payload, threads, duration, rate,
                 arrival) < 0) {
      std::snprintf(out, sizeof out, "ERR invalid SELFTEST args\n");
    }
    if (write(fd, out, std::strlen(out)) < 0) {}
    ::close(fd);
    return;
  }

  const char* usage =
      "ERR use: RUN <req> <reply> <conc> <dur> <warmup> <threads> | "
      "OPEN <req> <reply> <threads> <dur> <warmup> <rate> [const|poisson] "
      "[channels] | "
      "SELFTEST <payload> <threads> <dur> <rate> <const|poisson> | PING\n";
  if (write(fd, usage, std::strlen(usage)) < 0) {}
  ::close(fd);
}

}  // namespace

int main() {
  if (const char* t = std::getenv("BENCH_TRANSPORT")) g_transport = t;
  if (const char* s = std::getenv("BENCH_DST_SERVICE")) g_service = s;
  if (const char* r = std::getenv("BENCH_REACTORS")) g_reactors = std::atoi(r);
  if (const char* t = std::getenv("BENCH_TARGET")) {
    const std::string target(t);
    const size_t colon = target.rfind(':');
    if (colon == std::string::npos) {
      g_host = target;
    } else {
      g_host = target.substr(0, colon);
      g_port = std::atoi(target.c_str() + colon + 1);
    }
  }
  if (g_reactors < 1) g_reactors = 1;

  if (g_transport == "dmesh") {
    DmeshRuntime::Options options;
    options.reactor_count = static_cast<size_t>(g_reactors);
    auto runtime =
        DmeshRuntime::Create(dpumesh::grpc::MakeNativeDmeshApiOps(), options);
    if (!runtime.ok()) {
      std::fprintf(stderr, "[bench_grpc] runtime init failed: %s\n",
                   runtime.status().ToString().c_str());
      return 1;
    }
    g_runtime = std::move(*runtime);
  } else if (g_transport != "tcp") {
    std::fprintf(stderr, "[bench_grpc] BENCH_TRANSPORT must be dmesh or tcp\n");
    return 1;
  }

  const int port = std::getenv("CTRL_PORT") ? std::atoi(std::getenv("CTRL_PORT"))
                                            : kCtrlPortDefault;
  const int server = CtrlListen(port);
  if (server < 0) {
    std::fprintf(stderr, "[bench_grpc] control listen failed on :%d\n", port);
    return 1;
  }
  std::fprintf(stderr,
               "[bench_grpc] control LISTEN on :%d transport=%s reactors=%d\n",
               port, g_transport.c_str(), g_reactors);

  for (;;) {
    const int fd = ::accept(server, nullptr, nullptr);
    if (fd < 0) continue;
    HandleControl(fd);
  }
}
