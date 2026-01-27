/**
 * @file tcp_flow.h
 * @brief Class to abstract the components and functionality of a single TCP
 * flow.
 */

#ifndef SRC_INCLUDE_TCP_FLOW_H_
#define SRC_INCLUDE_TCP_FLOW_H_

#include <channel.h>
#include <channel_msgbuf.h>
#include <common.h>
#include <dpdk.h>
#include <ether.h>
#include <flow_key.h>
#include <glog/logging.h>
#include <ipv4.h>
#include <machnet_common.h>
#include <packet.h>
#include <packet_pool.h>
#include <pmd.h>
#include <tcp.h>
#include <tcp_cc.h>
#include <types.h>
#include <utils.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>

namespace juggler {
namespace net {
namespace tcp_flow {

/**
 * @class TXTracking
 * @brief Tracking for message buffers that are sent to the network.
 * Manages the chain of outgoing message buffers and their acknowledgment
 * status.
 */
class TXTracking {
 public:
  TXTracking() = delete;
  explicit TXTracking(shm::Channel* channel)
      : channel_(CHECK_NOTNULL(channel)),
        oldest_unacked_msgbuf_(nullptr),
        oldest_unsent_msgbuf_(nullptr),
        last_msgbuf_(nullptr),
        num_unsent_msgbufs_(0),
        num_tracked_msgbufs_(0) {}

  uint32_t NumUnsentMsgbufs() const { return num_unsent_msgbufs_; }
  shm::MsgBuf* GetOldestUnackedMsgBuf() const { return oldest_unacked_msgbuf_; }

  void ReceiveAcks(uint32_t num_acked_pkts) {
    shm::MsgBufBatch to_free;
    while (num_acked_pkts) {
      auto msgbuf = oldest_unacked_msgbuf_;
      DCHECK(msgbuf != nullptr);
      if (msgbuf != last_msgbuf_) {
        DCHECK_NE(oldest_unacked_msgbuf_, oldest_unsent_msgbuf_)
            << "Releasing an unsent msgbuf!";
        oldest_unacked_msgbuf_ = channel_->GetMsgBuf(msgbuf->next());
      } else {
        oldest_unacked_msgbuf_ = nullptr;
        last_msgbuf_ = nullptr;
      }
      to_free.Append(msgbuf, msgbuf->index());
      if (to_free.IsFull()) {
        num_tracked_msgbufs_ -= to_free.GetSize();
        CHECK(channel_->MsgBufBulkFree(&to_free));
      }
      num_acked_pkts--;
    }

    num_tracked_msgbufs_ -= to_free.GetSize();
    CHECK(channel_->MsgBufBulkFree(&to_free));
  }

  void Append(shm::MsgBuf* msgbuf) {
    DCHECK(msgbuf->is_first());
    // Append the message at the end of the chain of buffers, if any.
    if (last_msgbuf_ == nullptr) {
      // This is the first pending message buffer in the flow.
      DCHECK(oldest_unsent_msgbuf_ == nullptr);
      last_msgbuf_ = channel_->GetMsgBuf(msgbuf->last());
      oldest_unsent_msgbuf_ = msgbuf;
      oldest_unacked_msgbuf_ = msgbuf;
    } else {
      // This is not the first message buffer in the flow.
      DCHECK(oldest_unacked_msgbuf_ != nullptr);
      // Let's enqueue the new message buffer at the end of the chain.
      last_msgbuf_->link(msgbuf);
      DCHECK(!(last_msgbuf_->is_last() && last_msgbuf_->is_sg()));
      // Update the last buffer pointer to point to the current buffer.
      last_msgbuf_ = channel_->GetMsgBuf(msgbuf->last());
      if (oldest_unsent_msgbuf_ == nullptr) oldest_unsent_msgbuf_ = msgbuf;
    }

    const auto msg_length = msgbuf->msg_length();
    const auto effective_buffer_size = channel_->GetUsableBufSize();
    const auto msg_buffers_nr =
        (msg_length + effective_buffer_size - 1) / effective_buffer_size;
    num_unsent_msgbufs_ += msg_buffers_nr;
    num_tracked_msgbufs_ += msg_buffers_nr;
  }

  std::optional<shm::MsgBuf*> GetAndUpdateOldestUnsent() {
    if (oldest_unsent_msgbuf_ == nullptr) {
      DCHECK_EQ(NumUnsentMsgbufs(), 0);
      return std::nullopt;
    }

    auto msgbuf = oldest_unsent_msgbuf_;
    if (oldest_unsent_msgbuf_ != last_msgbuf_) {
      oldest_unsent_msgbuf_ =
          channel_->GetMsgBuf(oldest_unsent_msgbuf_->next());
    } else {
      oldest_unsent_msgbuf_ = nullptr;
    }

    num_unsent_msgbufs_--;
    return msgbuf;
  }

 private:
  uint32_t NumTrackedMsgbufs() const { return num_tracked_msgbufs_; }
  const shm::MsgBuf* GetLastMsgBuf() const { return last_msgbuf_; }
  const shm::MsgBuf* GetOldestUnsentMsgBuf() const {
    return oldest_unsent_msgbuf_;
  }

  shm::Channel* channel_;
  shm::MsgBuf* oldest_unacked_msgbuf_;
  shm::MsgBuf* oldest_unsent_msgbuf_;
  shm::MsgBuf* last_msgbuf_;
  uint32_t num_unsent_msgbufs_;
  uint32_t num_tracked_msgbufs_;
};

/**
 * @class RXTracking
 * @brief Tracking for message buffers received from the network.
 * Handles out-of-order reception and delivers complete messages to the
 * application.
 */
class RXTracking {
 public:
  struct reasm_queue_ent_t {
    shm::MsgBuf* msgbuf;
    uint32_t seqno;

    reasm_queue_ent_t(shm::MsgBuf* m, uint32_t s) : msgbuf(m), seqno(s) {}
  };

  static constexpr std::size_t kReassemblyMaxSeqnoDistance = 256;

  RXTracking(const RXTracking&) = delete;
  RXTracking(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
             uint16_t remote_port, shm::Channel* channel)
      : local_ip_(local_ip),
        local_port_(local_port),
        remote_ip_(remote_ip),
        remote_port_(remote_port),
        channel_(CHECK_NOTNULL(channel)),
        cur_msg_train_head_(nullptr),
        cur_msg_train_tail_(nullptr) {}

  /**
   * @brief Consume a TCP data packet.
   * @param pcb TCP control block.
   * @param packet Pointer to the received packet.
   * @param payload_offset Offset to the payload in the packet.
   * @param payload_len Length of the payload.
   * @return 0 on success, -1 on failure.
   */
  int Consume(tcp::TcpControlBlock* pcb, const dpdk::Packet* packet,
              size_t payload_offset, size_t payload_len) {
    const auto* tcph = packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    const auto* payload = packet->head_data<uint8_t*>(payload_offset);
    const auto seqno = tcph->seq_num.value();
    const auto expected_seqno = pcb->rcv_nxt;

    if (tcp::seqno_lt(seqno, expected_seqno)) {
      VLOG(2) << "Received old packet: " << seqno << " < " << expected_seqno;
      return 0;
    }

    const size_t distance = seqno - expected_seqno;
    if (distance >= kReassemblyMaxSeqnoDistance) {
      LOG(ERROR) << "Packet too far ahead. seqno: " << seqno
                 << ", expected: " << expected_seqno;
      return 0;
    }

    // Check for duplicates in out-of-order case
    auto it = reass_q_.begin();
    if (seqno != expected_seqno) {
      it = std::find_if(reass_q_.begin(), reass_q_.end(),
                        [&seqno](const reasm_queue_ent_t& entry) {
                          return entry.seqno >= seqno;
                        });
      if (it != reass_q_.end() && it->seqno == seqno) {
        return 0;  // Duplicate packet
      }
    }

    // Allocate buffer in SHM channel
    auto* msgbuf = channel_->MsgBufAlloc();
    if (msgbuf == nullptr) {
      VLOG(1) << "Failed to allocate message buffer. Dropping packet.";
      return -1;
    }

    auto* msg_data = msgbuf->append<uint8_t*>(payload_len);
    utils::Copy(CHECK_NOTNULL(msg_data), payload, payload_len);
    msgbuf->set_src_ip(remote_ip_);
    msgbuf->set_src_port(remote_port_);
    msgbuf->set_dst_ip(local_ip_);
    msgbuf->set_dst_port(local_port_);

    if (seqno == expected_seqno) {
      reass_q_.emplace_front(msgbuf, seqno);
    } else {
      reass_q_.insert(it, reasm_queue_ent_t(msgbuf, seqno));
    }

    // Add SACK block for out-of-order packet
    if (seqno != expected_seqno) {
      pcb->AddSackBlock(seqno, seqno + payload_len);
    }

    PushInOrderMsgbufsToShmTrain(pcb, payload_len);
    return 0;
  }

 private:
  void PushInOrderMsgbufsToShmTrain(tcp::TcpControlBlock* pcb,
                                    size_t payload_len) {
    while (!reass_q_.empty() && reass_q_.front().seqno == pcb->rcv_nxt) {
      auto& front = reass_q_.front();
      auto* msgbuf = front.msgbuf;
      reass_q_.pop_front();

      if (cur_msg_train_head_ == nullptr) {
        DCHECK(msgbuf->is_first());
        cur_msg_train_head_ = msgbuf;
        cur_msg_train_tail_ = msgbuf;
      } else {
        cur_msg_train_tail_->set_next(msgbuf);
        cur_msg_train_tail_ = msgbuf;
      }

      if (cur_msg_train_tail_->is_last()) {
        // Complete message, deliver to application
        DCHECK(!cur_msg_train_tail_->is_sg());
        auto* msgbuf_to_deliver = cur_msg_train_head_;
        auto nr_delivered = channel_->EnqueueMessages(&msgbuf_to_deliver, 1);
        if (nr_delivered != 1) {
          LOG(FATAL) << "SHM channel full, failed to deliver message";
        }

        cur_msg_train_head_ = nullptr;
        cur_msg_train_tail_ = nullptr;
      }

      // Advance rcv_nxt by payload length
      pcb->rcv_nxt += payload_len;
      pcb->ClearSackBlocks();
    }
  }

  const uint32_t local_ip_;
  const uint16_t local_port_;
  const uint32_t remote_ip_;
  const uint16_t remote_port_;
  shm::Channel* channel_;
  std::deque<reasm_queue_ent_t> reass_q_;
  shm::MsgBuf* cur_msg_train_head_;
  shm::MsgBuf* cur_msg_train_tail_;
};

/**
 * @class TcpFlow
 * @brief A TCP flow representing a connection between local and remote
 * endpoints.
 *
 * Manages TCP connection state, congestion control, and data transfer
 * using the TCP protocol instead of UDP.
 */
class TcpFlow {
 public:
  using Ethernet = net::Ethernet;
  using Ipv4 = net::Ipv4;
  using Tcp = net::Tcp;
  using ApplicationCallback =
      std::function<void(shm::Channel*, bool, const Key&)>;

  /**
   * @brief Construct a new TCP flow.
   *
   * @param local_addr Local IP address.
   * @param local_port Local TCP port.
   * @param remote_addr Remote IP address.
   * @param remote_port Remote TCP port.
   * @param local_l2_addr Local L2 address.
   * @param remote_l2_addr Remote L2 address.
   * @param txring TX ring to send packets to.
   * @param channel Shared memory channel this flow is associated with.
   */
  TcpFlow(const Ipv4::Address& local_addr, const Tcp::Port& local_port,
          const Ipv4::Address& remote_addr, const Tcp::Port& remote_port,
          const Ethernet::Address& local_l2_addr,
          const Ethernet::Address& remote_l2_addr, dpdk::TxRing* txring,
          ApplicationCallback callback, shm::Channel* channel)
      : key_(local_addr, local_port, remote_addr, remote_port),
        local_l2_addr_(local_l2_addr),
        remote_l2_addr_(remote_l2_addr),
        state_(tcp::TcpState::kClosed),
        txring_(CHECK_NOTNULL(txring)),
        callback_(std::move(callback)),
        channel_(CHECK_NOTNULL(channel)),
        pcb_(),
        tx_tracking_(CHECK_NOTNULL(channel)),
        rx_tracking_(local_addr.address.value(), local_port.port.value(),
                     remote_addr.address.value(), remote_port.port.value(),
                     CHECK_NOTNULL(channel)) {
    CHECK_NOTNULL(txring_->GetPacketPool());
    // Initialize PCB with random ISN
    pcb_.Initialize(GenerateISN());
  }

  ~TcpFlow() = default;

  bool operator==(const TcpFlow& other) const { return key_ == other.key(); }

  const Key& key() const { return key_; }
  shm::Channel* channel() const { return channel_; }
  tcp::TcpState state() const { return state_; }

  std::string ToString() const {
    return utils::Format(
        "%s [%s] <-> [%s]\n\t\t\t%s\n\t\t\t[TX Queue] Pending MsgBufs: %u",
        key_.ToString().c_str(), tcp::TcpStateToString(state_),
        channel_->GetName().c_str(), pcb_.ToString().c_str(),
        tx_tracking_.NumUnsentMsgbufs());
  }

  bool Match(const dpdk::Packet* packet) const {
    const auto* ih = packet->head_data<Ipv4*>(sizeof(Ethernet));
    const auto* tcph =
        packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));

    return (ih->src_addr == key_.remote_addr &&
            ih->dst_addr == key_.local_addr &&
            tcph->src_port == key_.remote_port &&
            tcph->dst_port == key_.local_port);
  }

  bool Match(const shm::MsgBuf* tx_msgbuf) const {
    const auto* flow_info = tx_msgbuf->flow();
    return (flow_info->src_ip == key_.local_addr.address.value() &&
            flow_info->dst_ip == key_.remote_addr.address.value() &&
            flow_info->src_port == key_.local_port.port.value() &&
            flow_info->dst_port == key_.remote_port.port.value());
  }

  /**
   * @brief Initiate TCP three-way handshake (active open).
   */
  void InitiateHandshake() {
    CHECK(state_ == tcp::TcpState::kClosed);
    SendSyn();
    state_ = tcp::TcpState::kSynSent;
    pcb_.RtoEnable();
  }

  /**
   * @brief Shutdown the connection.
   */
  void ShutDown() {
    switch (state_) {
      case tcp::TcpState::kClosed:
        break;
      case tcp::TcpState::kEstablished:
        SendFin();
        state_ = tcp::TcpState::kFinWait1;
        break;
      case tcp::TcpState::kCloseWait:
        SendFin();
        state_ = tcp::TcpState::kLastAck;
        break;
      default:
        // Send RST and close
        SendRst();
        state_ = tcp::TcpState::kClosed;
        break;
    }
    pcb_.RtoDisable();
  }

  /**
   * @brief Process an incoming TCP packet.
   * @param packet Pointer to the received packet.
   */
  void InputPacket(const dpdk::Packet* packet) {
    const auto* tcph =
        packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    const uint8_t tcp_flags = tcph->GetFlags();
    const uint32_t seq = tcph->seq_num.value();
    const uint32_t ack = tcph->ack_num.value();
    const uint16_t window = tcph->window.value();

    // Update send window
    pcb_.UpdateSndWnd(window);

    // Handle RST
    if (tcp_flags & Tcp::kRst) {
      HandleRst(seq);
      return;
    }

    switch (state_) {
      case tcp::TcpState::kSynSent:
        HandleSynSent(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kSynReceived:
        HandleSynReceived(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kEstablished:
        HandleEstablished(packet, tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kFinWait1:
        HandleFinWait1(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kFinWait2:
        HandleFinWait2(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kCloseWait:
        HandleCloseWait(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kClosing:
        HandleClosing(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kLastAck:
        HandleLastAck(tcph, tcp_flags, seq, ack);
        break;
      case tcp::TcpState::kTimeWait:
        // In TIME_WAIT, respond to any valid segment with ACK
        if (tcp_flags & Tcp::kAck) SendAck();
        break;
      default:
        break;
    }
  }

  /**
   * @brief Push a message from the application to the egress queue.
   * @param msg Pointer to the first message buffer.
   */
  void OutputMessage(shm::MsgBuf* msg) {
    tx_tracking_.Append(msg);
    TransmitPackets();
  }

  /**
   * @brief Periodic check for timeouts and retransmissions.
   * @return True if flow should continue, false if should be removed.
   */
  bool PeriodicCheck() {
    if (state_ == tcp::TcpState::kClosed) return false;

    if (pcb_.RtoDisabled()) return true;

    pcb_.RtoAdvance();
    if (pcb_.MaxRexmitsReached()) {
      if (state_ == tcp::TcpState::kSynSent) {
        LOG(INFO) << "TCP Flow " << this << " failed to establish";
        callback_(channel(), false, key());
      }
      return false;
    }

    if (pcb_.RtoExpired()) {
      RTORetransmit();
    }

    // Check for fast retransmit
    if (pcb_.ShouldFastRetransmit()) {
      FastRetransmit();
    }

    return true;
  }

 private:
  /**
   * @brief Generate Initial Sequence Number (ISN).
   */
  static uint32_t GenerateISN() {
    // Simple ISN generation - in production use RFC 6528
    return static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFF);
  }

  void PrepareL2Header(dpdk::Packet* packet) const {
    auto* eh = packet->head_data<Ethernet*>();
    eh->src_addr = local_l2_addr_;
    eh->dst_addr = remote_l2_addr_;
    eh->eth_type = be16_t(Ethernet::kIpv4);
    packet->set_l2_len(sizeof(*eh));
  }

  void PrepareL3Header(dpdk::Packet* packet) const {
    auto* ipv4h = packet->head_data<Ipv4*>(sizeof(Ethernet));
    ipv4h->version_ihl = 0x45;
    ipv4h->type_of_service = 0;
    ipv4h->packet_id = be16_t(0x1513);
    ipv4h->fragment_offset = be16_t(0);
    ipv4h->time_to_live = 64;
    ipv4h->next_proto_id = Ipv4::Proto::kTcp;
    ipv4h->total_length = be16_t(packet->length() - sizeof(Ethernet));
    ipv4h->src_addr = key_.local_addr;
    ipv4h->dst_addr = key_.remote_addr;
    ipv4h->hdr_checksum = 0;
    packet->set_l3_len(sizeof(*ipv4h));
  }

  void PrepareL4Header(dpdk::Packet* packet, uint32_t seq, uint32_t ack,
                       uint8_t flags) const {
    auto* tcph = packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    tcph->src_port = key_.local_port;
    tcph->dst_port = key_.remote_port;
    tcph->seq_num = be32_t(seq);
    tcph->ack_num = be32_t(ack);
    tcph->SetDataOffset(5);  // 20 bytes, no options
    tcph->SetFlags(flags);
    tcph->window = be16_t(pcb_.rcv_wnd);
    tcph->cksum = be16_t(0);
    tcph->urgent_ptr = be16_t(0);
    packet->offload_tcpv4_csum();
  }

  void SendControlPacket(uint32_t seq, uint32_t ack, uint8_t flags) const {
    auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
    dpdk::Packet::Reset(packet);

    const size_t kControlPacketSize =
        sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
    CHECK_NOTNULL(packet->append(kControlPacketSize));
    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet, seq, ack, flags);

    txring_->SendPackets(&packet, 1);
  }

  void SendSyn() {
    uint32_t seq = pcb_.GetAndAdvanceSndNxt();
    SendControlPacket(seq, 0, Tcp::kSyn);
  }

  void SendSynAck() {
    uint32_t seq = pcb_.GetAndAdvanceSndNxt();
    SendControlPacket(seq, pcb_.rcv_nxt, Tcp::kSyn | Tcp::kAck);
  }

  void SendAck() const {
    SendControlPacket(pcb_.snd_nxt, pcb_.rcv_nxt, Tcp::kAck);
  }

  void SendFin() {
    uint32_t seq = pcb_.GetAndAdvanceSndNxt();
    SendControlPacket(seq, pcb_.rcv_nxt, Tcp::kFin | Tcp::kAck);
  }

  void SendRst() const {
    SendControlPacket(pcb_.snd_nxt, pcb_.rcv_nxt, Tcp::kRst);
  }

  void HandleRst(uint32_t seq) {
    if (tcp::seqno_eq(seq, pcb_.rcv_nxt)) {
      state_ = tcp::TcpState::kClosed;
      callback_(channel(), false, key());
    }
  }

  void HandleSynSent(const Tcp* tcph, uint8_t flags, uint32_t seq,
                     uint32_t ack) {
    if ((flags & (Tcp::kSyn | Tcp::kAck)) == (Tcp::kSyn | Tcp::kAck)) {
      // SYN-ACK received
      if (ack != pcb_.snd_nxt) {
        LOG(ERROR) << "Invalid SYN-ACK ack number";
        SendRst();
        return;
      }
      pcb_.snd_una = ack;
      pcb_.rcv_nxt = seq + 1;
      pcb_.irs = seq;
      SendAck();
      state_ = tcp::TcpState::kEstablished;
      pcb_.RtoReset();
      callback_(channel(), true, key());
    } else if (flags & Tcp::kSyn) {
      // Simultaneous open - SYN received
      pcb_.rcv_nxt = seq + 1;
      pcb_.irs = seq;
      SendSynAck();
      state_ = tcp::TcpState::kSynReceived;
    }
  }

  void HandleSynReceived(const Tcp* tcph, uint8_t flags, uint32_t seq,
                         uint32_t ack) {
    if (flags & Tcp::kAck) {
      if (ack == pcb_.snd_nxt) {
        pcb_.snd_una = ack;
        state_ = tcp::TcpState::kEstablished;
        pcb_.RtoReset();
        callback_(channel(), true, key());
      }
    }
  }

  void HandleEstablished(const dpdk::Packet* packet, const Tcp* tcph,
                         uint8_t flags, uint32_t seq, uint32_t ack) {
    // Process ACK
    if (flags & Tcp::kAck) {
      bool is_dup = tcp::seqno_le(ack, pcb_.snd_una);
      pcb_.ProcessAck(ack, is_dup);
    }

    // Process data
    size_t hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + tcph->GetHeaderLength();
    size_t payload_len = packet->length() - hdr_len;
    if (payload_len > 0) {
      if (tcp::seqno_eq(seq, pcb_.rcv_nxt)) {
        rx_tracking_.Consume(&pcb_, packet, hdr_len, payload_len);
        SendAck();
      } else if (tcp::seqno_gt(seq, pcb_.rcv_nxt)) {
        // Out of order, buffer and send duplicate ACK
        rx_tracking_.Consume(&pcb_, packet, hdr_len, payload_len);
        SendAck();
      }
    }

    // Handle FIN
    if (flags & Tcp::kFin) {
      pcb_.rcv_nxt = seq + 1;
      SendAck();
      state_ = tcp::TcpState::kCloseWait;
    }

    // Try to send more data
    TransmitPackets();
  }

  void HandleFinWait1(const Tcp* tcph, uint8_t flags, uint32_t seq,
                      uint32_t ack) {
    if (flags & Tcp::kAck) {
      pcb_.ProcessAck(ack, false);
      if (flags & Tcp::kFin) {
        pcb_.rcv_nxt = seq + 1;
        SendAck();
        state_ = tcp::TcpState::kTimeWait;
      } else {
        state_ = tcp::TcpState::kFinWait2;
      }
    } else if (flags & Tcp::kFin) {
      pcb_.rcv_nxt = seq + 1;
      SendAck();
      state_ = tcp::TcpState::kClosing;
    }
  }

  void HandleFinWait2(const Tcp* tcph, uint8_t flags, uint32_t seq,
                      uint32_t ack) {
    if (flags & Tcp::kFin) {
      pcb_.rcv_nxt = seq + 1;
      SendAck();
      state_ = tcp::TcpState::kTimeWait;
    }
  }

  void HandleCloseWait(const Tcp* tcph, uint8_t flags, uint32_t seq,
                       uint32_t ack) {
    if (flags & Tcp::kAck) {
      pcb_.ProcessAck(ack, false);
    }
  }

  void HandleClosing(const Tcp* tcph, uint8_t flags, uint32_t seq,
                     uint32_t ack) {
    if (flags & Tcp::kAck) {
      pcb_.ProcessAck(ack, false);
      state_ = tcp::TcpState::kTimeWait;
    }
  }

  void HandleLastAck(const Tcp* tcph, uint8_t flags, uint32_t seq,
                     uint32_t ack) {
    if (flags & Tcp::kAck) {
      state_ = tcp::TcpState::kClosed;
    }
  }

  void FastRetransmit() {
    auto* msgbuf = tx_tracking_.GetOldestUnackedMsgBuf();
    if (msgbuf == nullptr) return;

    auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
    PrepareDataPacket(msgbuf, packet, pcb_.snd_una);
    txring_->SendPackets(&packet, 1);
    pcb_.RtoReset();
    LOG(INFO) << "Fast retransmitting TCP packet " << pcb_.snd_una;
  }

  void RTORetransmit() {
    pcb_.OnTimeout();

    if (state_ == tcp::TcpState::kEstablished) {
      auto* msgbuf = tx_tracking_.GetOldestUnackedMsgBuf();
      if (msgbuf != nullptr) {
        auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
        PrepareDataPacket(msgbuf, packet, pcb_.snd_una);
        txring_->SendPackets(&packet, 1);
        LOG(INFO) << "RTO retransmitting TCP data packet " << pcb_.snd_una;
      }
    } else if (state_ == tcp::TcpState::kSynSent) {
      SendSyn();
      LOG(INFO) << "RTO retransmitting SYN packet";
    } else if (state_ == tcp::TcpState::kSynReceived) {
      SendSynAck();
      LOG(INFO) << "RTO retransmitting SYN-ACK packet";
    }
    pcb_.RtoReset();
  }

  void PrepareDataPacket(shm::MsgBuf* msg_buf, dpdk::Packet* packet,
                         uint32_t seqno) const {
    const size_t hdr_length = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
    const uint32_t pkt_len = hdr_length + msg_buf->length();
    CHECK_LE(pkt_len - sizeof(Ethernet), dpdk::PmdRing::kDefaultFrameSize);

    dpdk::Packet::Reset(packet);
    CHECK_NOTNULL(packet->append(pkt_len));

    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet, seqno, pcb_.rcv_nxt, Tcp::kAck | Tcp::kPsh);

    // Copy payload
    auto* payload =
        packet->head_data<uint8_t*>(sizeof(Ethernet) + sizeof(Ipv4) +
                                    sizeof(Tcp));
    utils::Copy(payload, msg_buf->head_data(), msg_buf->length());
  }

  void TransmitPackets() {
    auto effective_wnd = pcb_.EffectiveWindow() / pcb_.mss;
    auto remaining_packets =
        std::min(effective_wnd, tx_tracking_.NumUnsentMsgbufs());
    if (remaining_packets == 0) return;

    do {
      dpdk::PacketBatch batch;
      auto pkt_cnt =
          std::min(remaining_packets, static_cast<uint32_t>(batch.GetRoom()));
      if (!txring_->GetPacketPool()->PacketBulkAlloc(&batch, pkt_cnt)) {
        LOG(ERROR) << "Failed to allocate packet batch";
        return;
      }

      for (uint16_t i = 0; i < batch.GetSize(); i++) {
        auto msg = tx_tracking_.GetAndUpdateOldestUnsent();
        if (!msg.has_value()) break;
        auto* msg_buf = msg.value();
        auto* packet = batch.pkts()[i];
        PrepareDataPacket(msg_buf, packet, pcb_.GetAndAdvanceSndNxt(msg_buf->length()));
      }

      txring_->SendPackets(&batch);
      remaining_packets -= pkt_cnt;
    } while (remaining_packets);

    if (pcb_.RtoDisabled()) pcb_.RtoEnable();
  }

  const Key key_;
  const Ethernet::Address local_l2_addr_;
  const Ethernet::Address remote_l2_addr_;
  tcp::TcpState state_;
  dpdk::TxRing* txring_;
  ApplicationCallback callback_;
  shm::Channel* channel_;
  tcp::TcpControlBlock pcb_;
  TXTracking tx_tracking_;
  RXTracking rx_tracking_;
};

}  // namespace tcp_flow
}  // namespace net
}  // namespace juggler

namespace std {

template <>
struct hash<juggler::net::tcp_flow::TcpFlow> {
  size_t operator()(const juggler::net::tcp_flow::TcpFlow& flow) const {
    const auto& key = flow.key();
    return juggler::utils::hash<uint64_t>(reinterpret_cast<const char*>(&key),
                                          sizeof(key));
  }
};

}  // namespace std

#endif  // SRC_INCLUDE_TCP_FLOW_H_
