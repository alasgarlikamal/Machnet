/**
 * @file tcp.h
 * @brief TCP (Transmission Control Protocol) header definition.
 */

#ifndef SRC_INCLUDE_TCP_H_
#define SRC_INCLUDE_TCP_H_

#include <types.h>
#include <utils.h>

#include <cstdint>

namespace juggler {
namespace net {

/**
 * @struct Tcp
 * @brief TCP header structure (RFC 793).
 *
 * The TCP header is 20 bytes without options:
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |          Source Port          |       Destination Port        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                        Sequence Number                        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                    Acknowledgment Number                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Data |           |U|A|P|R|S|F|                               |
 * | Offset| Reserved  |R|C|S|S|Y|I|            Window             |
 * |       |           |G|K|H|T|N|N|                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Checksum            |         Urgent Pointer        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct __attribute__((packed)) Tcp {
  static constexpr uint8_t kMinHeaderLen = 20;
  static constexpr uint8_t kMaxHeaderLen = 60;

  /**
   * @struct Port
   * @brief TCP port wrapper (identical to UDP port structure for consistency).
   */
  struct __attribute__((packed)) Port {
    static const uint8_t kSize = 2;
    Port() = default;
    Port(uint16_t tcp_port) { port = be16_t(tcp_port); }
    bool operator==(const Port &rhs) const { return port == rhs.port; }
    bool operator==(be16_t rhs) const { return rhs == port; }
    bool operator!=(const Port &rhs) const { return port != rhs.port; }
    bool operator!=(be16_t rhs) const { return rhs != port; }

    be16_t port;
  };

  /**
   * @enum Flags
   * @brief TCP control flags.
   */
  enum Flags : uint8_t {
    kFin = 0x01,  // Finish - no more data from sender
    kSyn = 0x02,  // Synchronize - initiate connection
    kRst = 0x04,  // Reset - abort connection
    kPsh = 0x08,  // Push - push data to application
    kAck = 0x10,  // Acknowledgment field is valid
    kUrg = 0x20,  // Urgent pointer field is valid
    kEce = 0x40,  // ECN-Echo (RFC 3168)
    kCwr = 0x80,  // Congestion Window Reduced (RFC 3168)
  };

  /**
   * @brief Get the data offset (header length) in 32-bit words.
   * @return Header length in 32-bit words (5-15).
   */
  uint8_t GetDataOffset() const { return (data_offset_reserved >> 4) & 0x0F; }

  /**
   * @brief Get the header length in bytes.
   * @return Header length in bytes (20-60).
   */
  uint8_t GetHeaderLength() const { return GetDataOffset() * 4; }

  /**
   * @brief Set the data offset (header length) in 32-bit words.
   * @param offset Header length in 32-bit words (5-15).
   */
  void SetDataOffset(uint8_t offset) {
    data_offset_reserved = (offset << 4) | (data_offset_reserved & 0x0F);
  }

  /**
   * @brief Get TCP flags.
   * @return TCP flags byte.
   */
  uint8_t GetFlags() const { return flags; }

  /**
   * @brief Set TCP flags.
   * @param f Flags to set.
   */
  void SetFlags(uint8_t f) { flags = f; }

  /**
   * @brief Check if a specific flag is set.
   * @param flag Flag to check.
   * @return true if flag is set, false otherwise.
   */
  bool HasFlag(Flags flag) const { return (flags & flag) != 0; }

  /**
   * @brief Set a specific flag.
   * @param flag Flag to set.
   */
  void SetFlag(Flags flag) { flags |= flag; }

  /**
   * @brief Clear a specific flag.
   * @param flag Flag to clear.
   */
  void ClearFlag(Flags flag) { flags &= ~flag; }

  /**
   * @brief Convert TCP header to human-readable string.
   * @return String representation of the TCP header.
   */
  std::string ToString() const;

  /**
   * @brief Get string representation of TCP flags.
   * @return String with flag names.
   */
  std::string FlagsToString() const;

  // TCP Header Fields
  Port src_port;              // Source port
  Port dst_port;              // Destination port
  be32_t seq_num;             // Sequence number
  be32_t ack_num;             // Acknowledgment number
  uint8_t data_offset_reserved;  // Data offset (4 bits) + Reserved (4 bits)
  uint8_t flags;              // Control flags
  be16_t window;              // Window size
  be16_t cksum;               // Checksum
  be16_t urgent_ptr;          // Urgent pointer
};

static_assert(sizeof(Tcp) == 20, "TCP header size must be 20 bytes");

/**
 * @brief Bitwise OR operator for TCP flags.
 */
inline Tcp::Flags operator|(Tcp::Flags lhs, Tcp::Flags rhs) {
  return static_cast<Tcp::Flags>(static_cast<uint8_t>(lhs) |
                                 static_cast<uint8_t>(rhs));
}

/**
 * @brief Bitwise AND operator for TCP flags.
 */
inline Tcp::Flags operator&(Tcp::Flags lhs, Tcp::Flags rhs) {
  return static_cast<Tcp::Flags>(static_cast<uint8_t>(lhs) &
                                 static_cast<uint8_t>(rhs));
}

}  // namespace net
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::net::Tcp::Port> {
  std::size_t operator()(const juggler::net::Tcp::Port &port) const {
    return juggler::utils::hash<uint32_t>(
        reinterpret_cast<const char *>(&port.port),
        sizeof(port.port.raw_value()));
  }
};
}  // namespace std

#endif  // SRC_INCLUDE_TCP_H_
