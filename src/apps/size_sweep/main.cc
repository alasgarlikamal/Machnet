/**
 * @file main.cc
 * @brief Size sweep: cycles through a configurable list of message sizes and
 * runs each size for a fixed duration, printing throughput and latency per
 * size. Produces a throughput-vs-size curve in a single run.
 *
 * Client flags: --local_ip, --remote_ip, --remote_port, --step_secs,
 *               --sizes (comma-separated, e.g. "64,256,1024,4096,65536")
 * Server mode:  omit --remote_ip
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <hdr/hdr_histogram.h>
#include <machnet.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::time_point;
using std::chrono::seconds;

DEFINE_string(local_ip, "", "IP of the local Machnet interface");
DEFINE_string(remote_ip, "", "IP of the remote server's Machnet interface");
DEFINE_uint32(remote_port, 888, "Remote port to connect to.");
DEFINE_uint32(local_port, 888, "Local port to listen on.");
DEFINE_uint32(msg_window, 8, "Max messages in flight per size step.");
DEFINE_uint32(step_secs, 5, "Seconds to run at each message size.");
DEFINE_string(sizes, "64,256,1024,4096,16384,65536",
              "Comma-separated list of message sizes to sweep.");

static volatile int g_keep_running = 1;

struct app_hdr_t {
  uint32_t msg_size;   // echoed back so client can verify
  uint32_t seq;
};

void SigIntHandler([[maybe_unused]] int signal) { g_keep_running = 0; }

static std::vector<uint32_t> parse_sizes(const std::string &s) {
  std::vector<uint32_t> out;
  std::istringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    uint32_t v = static_cast<uint32_t>(std::stoul(tok));
    CHECK_GT(v, sizeof(app_hdr_t)) << "Size " << v << " too small for header";
    CHECK_LE(v, (uint32_t)MACHNET_MSG_MAX_LEN) << "Size " << v << " exceeds MACHNET_MSG_MAX_LEN";
    out.push_back(v);
  }
  CHECK(!out.empty()) << "--sizes must not be empty";
  return out;
}

// ---- Server ----------------------------------------------------------------

void ServerLoop(void *channel_ctx) {
  std::vector<uint8_t> rx_buf(MACHNET_MSG_MAX_LEN);

  LOG(INFO) << "Server: ready, echoing all sizes.";

  while (g_keep_running) {
    MachnetFlow_t rx_flow;
    ssize_t n = machnet_recv(channel_ctx, rx_buf.data(), rx_buf.size(), &rx_flow);
    if (n <= 0) continue;

    MachnetFlow_t tx_flow;
    tx_flow.dst_ip   = rx_flow.src_ip;
    tx_flow.src_ip   = rx_flow.dst_ip;
    tx_flow.src_port = rx_flow.dst_port;
    tx_flow.dst_port = rx_flow.src_port;

    // Echo back exactly what we received
    machnet_send(channel_ctx, tx_flow, rx_buf.data(), n);
  }
}

// ---- Client ----------------------------------------------------------------

struct SizeResult {
  uint32_t msg_size;
  double   tx_mpps;
  double   tx_gbps;
  double   rtt_p50_us;
  double   rtt_p99_us;
  uint64_t drops;
};

static SizeResult run_one_size(void *channel_ctx, MachnetFlow_t *flow,
                               uint32_t msize, uint32_t window, uint32_t secs) {
  std::vector<uint8_t> tx_buf(msize, 0xab);
  std::vector<uint8_t> rx_buf(MACHNET_MSG_MAX_LEN);

  hdr_histogram *hist = nullptr;
  CHECK_EQ(hdr_init(1, 100'000'000, 2, &hist), 0);

  uint64_t tx_ok = 0, tx_drop = 0, rx_ok = 0;
  uint32_t in_flight = 0;
  uint32_t seq = 0;

  auto *hdr = reinterpret_cast<app_hdr_t *>(tx_buf.data());
  hdr->msg_size = msize;

  // Timestamps for in-flight messages (indexed by seq % window)
  std::vector<time_point<high_resolution_clock>> send_ts(window);

  auto deadline = high_resolution_clock::now() + seconds(secs);

  // Prime the window
  while (in_flight < window && g_keep_running) {
    hdr->seq = seq;
    send_ts[seq % window] = high_resolution_clock::now();
    if (machnet_send(channel_ctx, *flow, tx_buf.data(), msize) == 0) {
      in_flight++;
      seq++;
      tx_ok++;
    } else {
      tx_drop++;
    }
  }

  while (g_keep_running && high_resolution_clock::now() < deadline) {
    MachnetFlow_t rx_flow;
    ssize_t n = machnet_recv(channel_ctx, rx_buf.data(), rx_buf.size(), &rx_flow);
    if (n <= 0) continue;

    rx_ok++;
    in_flight--;

    const auto *rhdr = reinterpret_cast<const app_hdr_t *>(rx_buf.data());
    auto rtt_us = duration_cast<std::chrono::microseconds>(
        high_resolution_clock::now() - send_ts[rhdr->seq % window]).count();
    hdr_record_value(hist, rtt_us);

    // Send next
    hdr->seq = seq;
    send_ts[seq % window] = high_resolution_clock::now();
    if (machnet_send(channel_ctx, *flow, tx_buf.data(), msize) == 0) {
      in_flight++;
      seq++;
      tx_ok++;
    } else {
      tx_drop++;
    }
  }

  double elapsed = static_cast<double>(secs);
  SizeResult r;
  r.msg_size  = msize;
  r.tx_mpps   = tx_ok / (elapsed * 1e6);
  r.tx_gbps   = (tx_ok * msize * 8.0) / (elapsed * 1e9);
  r.rtt_p50_us = hdr_value_at_percentile(hist, 50.0);
  r.rtt_p99_us = hdr_value_at_percentile(hist, 99.0);
  r.drops     = tx_drop;

  hdr_close(hist);
  return r;
}

void ClientLoop(void *channel_ctx, MachnetFlow_t *flow) {
  const auto sizes = parse_sizes(FLAGS_sizes);
  const uint32_t window = FLAGS_msg_window;
  const uint32_t secs   = FLAGS_step_secs;

  // Print header
  std::cout << std::setw(10) << "Size(B)"
            << std::setw(12) << "TX(Mpps)"
            << std::setw(12) << "TX(Gbps)"
            << std::setw(14) << "RTT p50(us)"
            << std::setw(14) << "RTT p99(us)"
            << std::setw(10) << "Drops"
            << "\n"
            << std::string(72, '-') << "\n";

  for (uint32_t msize : sizes) {
    if (!g_keep_running) break;

    LOG(INFO) << "Running size=" << msize << "B for " << secs << "s";
    auto r = run_one_size(channel_ctx, flow, msize, window, secs);

    std::cout << std::setw(10) << r.msg_size
              << std::setw(12) << std::fixed << std::setprecision(3) << r.tx_mpps
              << std::setw(12) << std::fixed << std::setprecision(3) << r.tx_gbps
              << std::setw(14) << std::fixed << std::setprecision(1) << r.rtt_p50_us
              << std::setw(14) << std::fixed << std::setprecision(1) << r.rtt_p99_us
              << std::setw(10) << r.drops
              << "\n" << std::flush;
  }
}

// ---- main ------------------------------------------------------------------

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  signal(SIGINT, SigIntHandler);
  FLAGS_logtostderr = 1;

  CHECK_EQ(machnet_init(), 0);
  void *channel_ctx = machnet_attach();
  CHECK_NOTNULL(channel_ctx);

  std::thread datapath;

  if (FLAGS_remote_ip.empty()) {
    LOG(INFO) << "Server mode on " << FLAGS_local_ip << ":" << FLAGS_local_port;
    CHECK_EQ(machnet_listen(channel_ctx, FLAGS_local_ip.c_str(), FLAGS_local_port), 0);
    datapath = std::thread(ServerLoop, channel_ctx);
    while (g_keep_running) sleep(1);
  } else {
    LOG(INFO) << "Client mode, connecting to "
              << FLAGS_remote_ip << ":" << FLAGS_remote_port;
    MachnetFlow_t flow;
    CHECK_EQ(machnet_connect(channel_ctx, FLAGS_local_ip.c_str(),
                             FLAGS_remote_ip.c_str(), FLAGS_remote_port, &flow), 0);
    datapath = std::thread(ClientLoop, channel_ctx, &flow);
  }

  datapath.join();
  return 0;
}
