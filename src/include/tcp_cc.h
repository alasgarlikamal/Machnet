/**
 * @file tcp_cc.h
 * @brief TCP Congestion Control implementation.
 *
 * This file implements standard TCP congestion control algorithms:
 * - Slow Start
 * - Congestion Avoidance
 * - Fast Retransmit
 * - Fast Recovery (RFC 5681)
 * - NewReno-style recovery
 */

#ifndef SRC_INCLUDE_TCP_CC_H_
#define SRC_INCLUDE_TCP_CC_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "utils.h"

namespace juggler {
namespace net {
namespace tcp {

/**
 * @brief Sequence number comparison functions for wrap-around handling.
 * Uses signed arithmetic to handle 32-bit sequence number wrap-around.
 */
constexpr bool seqno_lt(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) < 0;
}
constexpr bool seqno_le(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) <= 0;
}
constexpr bool seqno_eq(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) == 0;
}
constexpr bool seqno_ge(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) >= 0;
}
constexpr bool seqno_gt(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

/**
 * @enum TcpState
 * @brief TCP connection states (RFC 793).
 */
enum class TcpState : uint8_t {
  kClosed,
  kListen,
  kSynSent,
  kSynReceived,
  kEstablished,
  kFinWait1,
  kFinWait2,
  kCloseWait,
  kClosing,
  kLastAck,
  kTimeWait,
};

/**
 * @brief Convert TCP state to string.
 */
inline constexpr const char* TcpStateToString(TcpState state) {
  switch (state) {
    case TcpState::kClosed:
      return "CLOSED";
    case TcpState::kListen:
      return "LISTEN";
    case TcpState::kSynSent:
      return "SYN_SENT";
    case TcpState::kSynReceived:
      return "SYN_RECEIVED";
    case TcpState::kEstablished:
      return "ESTABLISHED";
    case TcpState::kFinWait1:
      return "FIN_WAIT_1";
    case TcpState::kFinWait2:
      return "FIN_WAIT_2";
    case TcpState::kCloseWait:
      return "CLOSE_WAIT";
    case TcpState::kClosing:
      return "CLOSING";
    case TcpState::kLastAck:
      return "LAST_ACK";
    case TcpState::kTimeWait:
      return "TIME_WAIT";
    default:
      return "UNKNOWN";
  }
}

/**
 * @enum CongestionState
 * @brief TCP congestion control states.
 */
enum class CongestionState : uint8_t {
  kSlowStart,          // Exponential cwnd growth
  kCongestionAvoidance,  // Linear cwnd growth (AIMD)
  kFastRecovery,       // After fast retransmit (NewReno)
};

/**
 * @brief Convert congestion state to string.
 */
inline constexpr const char* CongestionStateToString(CongestionState state) {
  switch (state) {
    case CongestionState::kSlowStart:
      return "SLOW_START";
    case CongestionState::kCongestionAvoidance:
      return "CONGESTION_AVOIDANCE";
    case CongestionState::kFastRecovery:
      return "FAST_RECOVERY";
    default:
      return "UNKNOWN";
  }
}

/**
 * @struct RttEstimator
 * @brief RTT estimation using Jacobson/Karels algorithm (RFC 6298).
 */
struct RttEstimator {
  static constexpr int64_t kInitialRttUs = 1000000;  // 1 second initial RTT
  static constexpr int64_t kMinRtoUs = 200000;       // 200ms minimum RTO
  static constexpr int64_t kMaxRtoUs = 60000000;     // 60 seconds maximum RTO
  static constexpr int kRtoClockGranularityUs = 1000;  // 1ms granularity

  // Alpha = 1/8, Beta = 1/4 (RFC 6298 recommended values)
  static constexpr int kAlphaShift = 3;  // 1/8
  static constexpr int kBetaShift = 2;   // 1/4

  int64_t srtt_us{0};           // Smoothed RTT (microseconds)
  int64_t rttvar_us{0};         // RTT variance (microseconds)
  int64_t rto_us{kInitialRttUs};  // Retransmission timeout (microseconds)
  bool has_measurement{false};

  /**
   * @brief Update RTT estimate with a new measurement.
   * @param rtt_us Measured RTT in microseconds.
   */
  void UpdateRtt(int64_t rtt_us) {
    if (!has_measurement) {
      // First RTT measurement
      srtt_us = rtt_us;
      rttvar_us = rtt_us / 2;
      has_measurement = true;
    } else {
      // Jacobson/Karels algorithm
      int64_t delta = rtt_us - srtt_us;
      srtt_us += delta >> kAlphaShift;
      int64_t abs_delta = delta >= 0 ? delta : -delta;
      rttvar_us += (abs_delta - rttvar_us) >> kBetaShift;
    }

    // RTO = SRTT + max(G, 4 * RTTVAR)
    int64_t k_rttvar = rttvar_us << 2;  // 4 * RTTVAR
    int64_t timeout = srtt_us + std::max(static_cast<int64_t>(kRtoClockGranularityUs), k_rttvar);
    rto_us = std::clamp(timeout, kMinRtoUs, kMaxRtoUs);
  }

  /**
   * @brief Apply exponential backoff to RTO (on timeout).
   */
  void BackoffRto() {
    rto_us = std::min(rto_us * 2, kMaxRtoUs);
  }

  /**
   * @brief Reset estimator to initial state.
   */
  void Reset() {
    srtt_us = 0;
    rttvar_us = 0;
    rto_us = kInitialRttUs;
    has_measurement = false;
  }

  std::string ToString() const {
    return utils::Format("srtt=%ldus, rttvar=%ldus, rto=%ldus",
                         srtt_us, rttvar_us, rto_us);
  }
};

/**
 * @struct TcpControlBlock
 * @brief TCP Protocol Control Block (PCB) for congestion control.
 *
 * Implements standard TCP congestion control with:
 * - Slow start with initial window
 * - Congestion avoidance (AIMD)
 * - Fast retransmit and fast recovery (NewReno)
 * - SACK support
 */
struct TcpControlBlock {
  // Constants
  static constexpr uint32_t kInitialCwnd = 10;        // Initial cwnd (RFC 6928)
  static constexpr uint32_t kMinCwnd = 2;             // Minimum cwnd
  static constexpr uint32_t kInitialSsthresh = 65535; // Initial ssthresh
  static constexpr uint32_t kDupAckThreshold = 3;     // Dup ACKs for fast retransmit
  static constexpr uint16_t kDefaultMss = 1460;       // Default MSS
  static constexpr std::size_t kSackBlocksMax = 4;    // Max SACK blocks
  static constexpr int kRtoDisabled = -1;
  static constexpr int kMaxRexmits = 15;              // Max retransmissions

  // Send sequence variables (RFC 793)
  uint32_t snd_una{0};     // Oldest unacknowledged sequence number
  uint32_t snd_nxt{0};     // Next sequence number to send
  uint32_t snd_wnd{65535}; // Send window (advertised by receiver)
  uint32_t iss{0};         // Initial send sequence number

  // Receive sequence variables
  uint32_t rcv_nxt{0};     // Next expected sequence number
  uint32_t rcv_wnd{65535}; // Receive window (advertised to sender)
  uint32_t irs{0};         // Initial receive sequence number

  // Congestion control variables
  uint32_t cwnd{kInitialCwnd * kDefaultMss};  // Congestion window (bytes)
  uint32_t ssthresh{kInitialSsthresh};        // Slow start threshold
  uint16_t mss{kDefaultMss};                  // Maximum segment size
  CongestionState cc_state{CongestionState::kSlowStart};

  // Fast retransmit/recovery state
  uint32_t dup_ack_count{0};      // Duplicate ACK counter
  uint32_t recover{0};            // Recovery point (NewReno)
  uint32_t high_rxt{0};           // Highest retransmitted sequence

  // SACK support
  struct SackBlock {
    uint32_t left{0};   // Left edge of SACK block
    uint32_t right{0};  // Right edge of SACK block
    bool valid{false};
  };
  SackBlock sack_blocks[kSackBlocksMax];
  uint8_t sack_block_count{0};

  // RTT estimation
  RttEstimator rtt_est;

  // RTO timer state
  int rto_timer{kRtoDisabled};
  int rto_rexmits{0};

  // Statistics
  uint64_t bytes_sent{0};
  uint64_t bytes_acked{0};
  uint64_t packets_retransmitted{0};

  /**
   * @brief Initialize the control block with an initial sequence number.
   * @param initial_seq Initial sequence number.
   */
  void Initialize(uint32_t initial_seq) {
    iss = initial_seq;
    snd_una = initial_seq;
    snd_nxt = initial_seq;
    cwnd = kInitialCwnd * mss;
    ssthresh = kInitialSsthresh;
    cc_state = CongestionState::kSlowStart;
    dup_ack_count = 0;
    rto_timer = kRtoDisabled;
    rto_rexmits = 0;
  }

  /**
   * @brief Get the current send sequence number and advance.
   * @return Current snd_nxt before advancing.
   */
  uint32_t GetAndAdvanceSndNxt(uint32_t len = 1) {
    uint32_t seq = snd_nxt;
    snd_nxt += len;
    return seq;
  }

  /**
   * @brief Calculate effective send window (min of cwnd and receiver window).
   * @return Effective window in bytes.
   */
  uint32_t EffectiveWindow() const {
    uint32_t wnd = std::min(cwnd, snd_wnd);
    uint32_t flight_size = snd_nxt - snd_una;
    return flight_size >= wnd ? 0 : wnd - flight_size;
  }

  /**
   * @brief Get the number of bytes in flight.
   * @return Bytes in flight.
   */
  uint32_t FlightSize() const { return snd_nxt - snd_una; }

  /**
   * @brief Process an ACK and update congestion control state.
   * @param ack_num Acknowledgment number from received ACK.
   * @param is_dup True if this is a duplicate ACK.
   * @return Number of newly acknowledged bytes.
   */
  uint32_t ProcessAck(uint32_t ack_num, bool is_dup) {
    uint32_t newly_acked = 0;

    if (is_dup) {
      return ProcessDuplicateAck(ack_num);
    }

    // Check for valid ACK
    if (seqno_gt(ack_num, snd_nxt)) {
      // ACK for data not yet sent - invalid
      return 0;
    }

    if (seqno_le(ack_num, snd_una)) {
      // Old/duplicate ACK
      return ProcessDuplicateAck(ack_num);
    }

    // New ACK - calculate bytes acknowledged
    newly_acked = ack_num - snd_una;
    snd_una = ack_num;
    bytes_acked += newly_acked;

    // Reset duplicate ACK counter
    dup_ack_count = 0;

    // Update congestion control based on state
    switch (cc_state) {
      case CongestionState::kSlowStart:
        OnAckSlowStart(newly_acked);
        break;
      case CongestionState::kCongestionAvoidance:
        OnAckCongestionAvoidance(newly_acked);
        break;
      case CongestionState::kFastRecovery:
        OnAckFastRecovery(ack_num, newly_acked);
        break;
    }

    // Reset RTO timer if there's still outstanding data
    if (snd_una == snd_nxt) {
      RtoDisable();
    } else {
      RtoReset();
    }
    rto_rexmits = 0;

    return newly_acked;
  }

  /**
   * @brief Handle a timeout event.
   */
  void OnTimeout() {
    // Set ssthresh to half of flight size (RFC 5681)
    ssthresh = std::max(FlightSize() / 2, 2 * mss);

    // Reset cwnd to 1 MSS (or IW for loss-based)
    cwnd = mss;

    // Enter slow start
    cc_state = CongestionState::kSlowStart;

    // Clear duplicate ACK count
    dup_ack_count = 0;

    // Backoff RTO
    rtt_est.BackoffRto();
    rto_rexmits++;
    packets_retransmitted++;
  }

  /**
   * @brief Check if fast retransmit should be triggered.
   * @return True if fast retransmit condition is met.
   */
  bool ShouldFastRetransmit() const {
    return dup_ack_count >= kDupAckThreshold &&
           cc_state != CongestionState::kFastRecovery;
  }

  /**
   * @brief Check if max retransmissions have been reached.
   */
  bool MaxRexmitsReached() const { return rto_rexmits >= kMaxRexmits; }

  // RTO timer management
  bool RtoDisabled() const { return rto_timer == kRtoDisabled; }
  bool RtoExpired() const {
    return rto_timer >= 0 &&
           static_cast<int64_t>(rto_timer) * 100000 >= rtt_est.rto_us;
  }
  void RtoEnable() { rto_timer = 0; }
  void RtoDisable() { rto_timer = kRtoDisabled; }
  void RtoReset() { rto_timer = 0; }
  void RtoAdvance() {
    if (rto_timer >= 0) rto_timer++;
  }

  /**
   * @brief Update receive window.
   * @param window Advertised window from sender.
   */
  void UpdateSndWnd(uint16_t window) { snd_wnd = window; }

  /**
   * @brief Update MSS.
   * @param new_mss New maximum segment size.
   */
  void UpdateMss(uint16_t new_mss) {
    mss = new_mss;
    // Adjust cwnd to be a multiple of MSS
    if (cwnd < mss) cwnd = mss;
  }

  /**
   * @brief Add a SACK block.
   * @param left Left edge of the block.
   * @param right Right edge of the block.
   */
  void AddSackBlock(uint32_t left, uint32_t right) {
    if (sack_block_count < kSackBlocksMax) {
      sack_blocks[sack_block_count].left = left;
      sack_blocks[sack_block_count].right = right;
      sack_blocks[sack_block_count].valid = true;
      sack_block_count++;
    }
  }

  /**
   * @brief Clear all SACK blocks.
   */
  void ClearSackBlocks() {
    for (size_t i = 0; i < kSackBlocksMax; i++) {
      sack_blocks[i].valid = false;
    }
    sack_block_count = 0;
  }

  std::string ToString() const {
    return utils::Format(
        "[TCP CC] snd_una=%u, snd_nxt=%u, cwnd=%u, ssthresh=%u, "
        "state=%s, dup_acks=%u, flight=%u, eff_wnd=%u, %s",
        snd_una, snd_nxt, cwnd, ssthresh,
        CongestionStateToString(cc_state), dup_ack_count,
        FlightSize(), EffectiveWindow(),
        rtt_est.ToString().c_str());
  }

 private:
  /**
   * @brief Handle ACK in slow start phase.
   */
  void OnAckSlowStart(uint32_t newly_acked) {
    // Exponential growth: increase cwnd by number of bytes ACKed
    cwnd += newly_acked;

    // Check if we should transition to congestion avoidance
    if (cwnd >= ssthresh) {
      cc_state = CongestionState::kCongestionAvoidance;
    }
  }

  /**
   * @brief Handle ACK in congestion avoidance phase.
   */
  void OnAckCongestionAvoidance(uint32_t newly_acked) {
    // Linear growth: increase cwnd by MSS^2/cwnd for each ACK
    // This approximates: cwnd += MSS per RTT
    uint32_t increment = (mss * newly_acked) / cwnd;
    if (increment == 0) increment = 1;
    cwnd += increment;
  }

  /**
   * @brief Handle ACK in fast recovery phase (NewReno).
   */
  void OnAckFastRecovery(uint32_t ack_num, uint32_t newly_acked) {
    if (seqno_ge(ack_num, recover)) {
      // Full ACK - exit fast recovery
      cwnd = std::min(ssthresh, FlightSize() + mss);
      cc_state = CongestionState::kCongestionAvoidance;
    } else {
      // Partial ACK - stay in fast recovery
      // Deflate cwnd by amount of new data ACKed
      cwnd -= newly_acked;
      // Add back one segment (to compensate for partial ACK)
      cwnd += mss;
      // Retransmit next unacked segment
      packets_retransmitted++;
    }
  }

  /**
   * @brief Handle duplicate ACK.
   * @return Bytes acknowledged (always 0 for dup ACKs).
   */
  uint32_t ProcessDuplicateAck(uint32_t ack_num) {
    dup_ack_count++;

    if (cc_state == CongestionState::kFastRecovery) {
      // In fast recovery, inflate cwnd
      cwnd += mss;
      return 0;
    }

    if (dup_ack_count == kDupAckThreshold) {
      // Enter fast retransmit/recovery
      EnterFastRecovery();
    }

    return 0;
  }

  /**
   * @brief Enter fast recovery mode.
   */
  void EnterFastRecovery() {
    // Set ssthresh to half of flight size
    ssthresh = std::max(FlightSize() / 2, 2 * mss);

    // Set recovery point
    recover = snd_nxt;

    // Set cwnd = ssthresh + 3*MSS (RFC 5681)
    cwnd = ssthresh + kDupAckThreshold * mss;

    // Enter fast recovery state
    cc_state = CongestionState::kFastRecovery;

    packets_retransmitted++;
  }
};

}  // namespace tcp
}  // namespace net
}  // namespace juggler

#endif  // SRC_INCLUDE_TCP_CC_H_
