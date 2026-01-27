/**
 * @file tcp.cc
 * @brief TCP header implementation.
 */

#include <tcp.h>

namespace juggler {
namespace net {

std::string Tcp::ToString() const {
  return juggler::utils::Format(
      "[TCP: src_port %zu, dst_port %zu, seq %u, ack %u, "
      "offset %u, flags %s, win %zu, cksum 0x%04x, urg %zu]",
      src_port.port.value(), dst_port.port.value(), seq_num.value(),
      ack_num.value(), GetDataOffset(), FlagsToString().c_str(),
      window.value(), cksum.value(), urgent_ptr.value());
}

std::string Tcp::FlagsToString() const {
  std::string result;
  if (HasFlag(kFin)) result += "FIN|";
  if (HasFlag(kSyn)) result += "SYN|";
  if (HasFlag(kRst)) result += "RST|";
  if (HasFlag(kPsh)) result += "PSH|";
  if (HasFlag(kAck)) result += "ACK|";
  if (HasFlag(kUrg)) result += "URG|";
  if (HasFlag(kEce)) result += "ECE|";
  if (HasFlag(kCwr)) result += "CWR|";

  // Remove trailing '|' if present
  if (!result.empty() && result.back() == '|') {
    result.pop_back();
  }

  if (result.empty()) {
    result = "NONE";
  }

  return result;
}

}  // namespace net
}  // namespace juggler
