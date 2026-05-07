/**
 * @file main.cc
 * @brief Burst sender: sends messages in back-to-back bursts then drains all
 * responses before starting the next burst. Useful for stressing the ring
 * under bursty load and measuring per-burst latency vs steady-state.
 *
 * Client flags: --local_ip, --remote_ip, --remote_port, --burst_size,
 *               --msg_size, --msg_nr
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
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>
#include "../barrier.h"

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::time_point;

DEFINE_string(local_ip, "", "IP of the local Machnet interface");
DEFINE_string(remote_ip, "", "IP of the remote server's Machnet interface");
DEFINE_uint32(remote_port, 888, "Remote port to connect to.");
DEFINE_uint32(local_port, 888, "Local port to listen on.");
DEFINE_uint32(msg_size, 64, "Message payload size in bytes.");
DEFINE_uint32(burst_size, 32, "Number of messages to send per burst.");
DEFINE_uint64(msg_nr, UINT64_MAX, "Total number of messages to send.");
DEFINE_string(barrier_dir, "", "Directory for client sync barrier.");
DEFINE_int32(n_clients, 1, "Total number of clients (for barrier).");

static volatile int g_keep_running = 1;

struct app_hdr_t {
  uint64_t burst_seq;   // which burst this message belongs to
  uint32_t msg_index;   // position within the burst
};

void SigIntHandler([[maybe_unused]] int signal) { g_keep_running = 0; }

// ---- Server ----------------------------------------------------------------

void ServerLoop(void *channel_ctx) {
  std::vector<uint8_t> rx_buf(MACHNET_MSG_MAX_LEN);
  std::vector<uint8_t> tx_buf(MACHNET_MSG_MAX_LEN);
  uint64_t rx_count = 0, tx_count = 0;
  auto last_report = high_resolution_clock::now();

  LOG(INFO) << "Server: listening, echoing bursts back.";

  while (g_keep_running) {
    MachnetFlow_t rx_flow;
    ssize_t n = machnet_recv(channel_ctx, rx_buf.data(), rx_buf.size(), &rx_flow);
    if (n <= 0) continue;
    rx_count++;

    // Echo back with swapped flow
    MachnetFlow_t tx_flow;
    tx_flow.dst_ip   = rx_flow.src_ip;
    tx_flow.src_ip   = rx_flow.dst_ip;
    tx_flow.src_port = rx_flow.dst_port;
    tx_flow.dst_port = rx_flow.src_port;

    memcpy(tx_buf.data(), rx_buf.data(), n);
    if (machnet_send(channel_ctx, tx_flow, tx_buf.data(), n) == 0)
      tx_count++;

    auto now = high_resolution_clock::now();
    double sec = duration_cast<std::chrono::nanoseconds>(now - last_report).count() * 1e-9;
    if (sec >= 1.0) {
      std::cout << "Server RX/TX: " << rx_count / sec << " / "
                << tx_count / sec << " msg/s\n";
      rx_count = tx_count = 0;
      last_report = now;
    }
  }
}

// ---- Client ----------------------------------------------------------------

void ClientLoop(void *channel_ctx, MachnetFlow_t *flow) {
  const uint32_t burst  = FLAGS_burst_size;
  const uint32_t msize  = FLAGS_msg_size;
  const uint64_t total  = FLAGS_msg_nr;

  CHECK_GT(msize, sizeof(app_hdr_t)) << "--msg_size must be > " << sizeof(app_hdr_t);

  std::vector<uint8_t> tx_buf(msize);
  std::vector<uint8_t> rx_buf(MACHNET_MSG_MAX_LEN);
  std::iota(tx_buf.begin(), tx_buf.end(), 0);

  // Per-burst latency histogram
  hdr_histogram *hist = nullptr;
  CHECK_EQ(hdr_init(1, 100'000'000, 2, &hist), 0);

  uint64_t bursts_sent = 0;
  uint64_t msgs_sent   = 0;
  uint64_t msgs_recvd  = 0;
  uint64_t tx_drops    = 0;

  auto last_report = high_resolution_clock::now();

  LOG(INFO) << "Client: burst_size=" << burst << " msg_size=" << msize;

  while (g_keep_running && msgs_sent < total) {
    // --- Send one full burst ---
    auto burst_start = high_resolution_clock::now();
    uint32_t sent_this_burst = 0;

    for (uint32_t i = 0; i < burst && msgs_sent + i < total; i++) {
      auto *hdr = reinterpret_cast<app_hdr_t *>(tx_buf.data());
      hdr->burst_seq = bursts_sent;
      hdr->msg_index = i;

      if (machnet_send(channel_ctx, *flow, tx_buf.data(), msize) == 0) {
        sent_this_burst++;
      } else {
        tx_drops++;
      }
    }

    msgs_sent += sent_this_burst;

    // --- Drain: wait for all responses from this burst ---
    uint32_t recvd_this_burst = 0;
    while (g_keep_running && recvd_this_burst < sent_this_burst) {
      MachnetFlow_t rx_flow;
      ssize_t n = machnet_recv(channel_ctx, rx_buf.data(), rx_buf.size(), &rx_flow);
      if (n <= 0) continue;
      recvd_this_burst++;
      msgs_recvd++;
    }

    // Record burst RTT (time from first send to last receive)
    auto burst_us = duration_cast<std::chrono::microseconds>(
        high_resolution_clock::now() - burst_start).count();
    hdr_record_value(hist, burst_us);
    bursts_sent++;

    // Per-second report
    auto now = high_resolution_clock::now();
    double sec = duration_cast<std::chrono::nanoseconds>(now - last_report).count() * 1e-9;
    if (sec >= 1.0) {
      auto p = hdr_value_at_percentile;
      std::cout << std::fixed << std::setprecision(1)
                << "Bursts/s: " << bursts_sent / sec
                << "  TX: " << msgs_sent / sec << " msg/s"
                << "  Burst RTT p50/p99 (us): "
                << p(hist, 50) << "/" << p(hist, 99)
                << "  Drops: " << tx_drops << std::endl;
      bursts_sent = msgs_sent = msgs_recvd = tx_drops = 0;
      hdr_reset(hist);
      last_report = now;
    }
  }

  hdr_close(hist);
}

// ---- main ------------------------------------------------------------------

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  signal(SIGINT, SigIntHandler);
  signal(SIGTERM, SigIntHandler);
  FLAGS_logtostderr = 1;

  CHECK_EQ(machnet_init(), 0);
  void *channel_ctx = machnet_attach();
  CHECK_NOTNULL(channel_ctx);

  std::thread datapath;

  if (FLAGS_remote_ip.empty()) {
    LOG(INFO) << "Server mode, listening on " << FLAGS_local_ip << ":" << FLAGS_local_port;
    int ret = machnet_listen(channel_ctx, FLAGS_local_ip.c_str(), FLAGS_local_port);
    CHECK_EQ(ret, 0) << "machnet_listen failed";
    datapath = std::thread(ServerLoop, channel_ctx);
  } else {
    LOG(INFO) << "Client mode, connecting to " << FLAGS_remote_ip << ":" << FLAGS_remote_port;
    MachnetFlow_t flow;
    int ret = machnet_connect(channel_ctx, FLAGS_local_ip.c_str(),
                              FLAGS_remote_ip.c_str(), FLAGS_remote_port, &flow);
    CHECK_EQ(ret, 0) << "machnet_connect failed";
    barrier_wait(FLAGS_barrier_dir, FLAGS_n_clients, g_keep_running);
    datapath = std::thread(ClientLoop, channel_ctx, &flow);
  }

  while (g_keep_running) sleep(1);
  datapath.join();
  return 0;
}
