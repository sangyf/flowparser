#include "flows.h"

namespace flowparser {

TCPRateEstimator::TCPRateEstimator(const Flow* flow)
    : flow_(flow),
      last_seen_seq_(std::numeric_limits<uint32_t>::max()),
      bytes_this_second_(0),
      curr_bytes_per_second_(0),
      curr_second_start_(flow->first_rx()),
      out_of_order_(false) {
}

void TCPRateEstimator::UpdateEstimate(uint32_t seq, uint32_t payload_size,
                                      uint64_t timestamp) {
  if (last_seen_seq_ == std::numeric_limits<uint32_t>::max()) {
    bytes_this_second_ += payload_size;
    last_seen_seq_ = seq + payload_size;
    return;
  }

  if (seq < last_seen_seq_) {
    // Frame is out of order. We ignore it.
    out_of_order_ = true;
    return;
  }

  const uint64_t bytes_delta = seq - last_seen_seq_ + payload_size;

  const uint64_t time_delta = timestamp - flow_->last_rx();
  const uint64_t curr_second_end = curr_second_start_ + kMillion;
  const double alpha = flow_->flow_config().tcp_estimator_ewma_alpha();

  if (timestamp <= curr_second_end) {
    bytes_this_second_ += bytes_delta;
  } else {
    double rate = static_cast<double>(bytes_delta) / time_delta;

    uint32_t seconds_skipped = (timestamp - curr_second_end) / kMillion;
    uint64_t time_remaining_until_end_of_second = curr_second_end
        - flow_->last_rx();

    // We know that a certain number of bytes (bytes_delta) were transmitted by
    // the flow over a period starting in the current second and ending in one
    // of the next seconds.
    bytes_this_second_ += rate * time_remaining_until_end_of_second;

    // If this is the first second we will not decay the Bps estimate.
    if (curr_second_start_ == flow_->first_rx()) {
      curr_bytes_per_second_ = bytes_this_second_;
    } else {
      curr_bytes_per_second_ = (1 - alpha) * curr_bytes_per_second_
          + alpha * bytes_this_second_;
    }

    // For all seconds that we have skipped we decay the value.
    for (size_t i = 0; i < seconds_skipped; ++i) {
      curr_bytes_per_second_ = (1 - alpha) * curr_bytes_per_second_
          + alpha * rate * kMillion;
    }

    uint64_t time_into_new_second = time_delta - (seconds_skipped * kMillion)
        + time_remaining_until_end_of_second;

    bytes_this_second_ = rate * time_into_new_second;
    curr_second_start_ = curr_second_end + seconds_skipped * kMillion;
  }

  last_seen_seq_ = seq + payload_size;
}

double TCPRateEstimator::GetBytesPerSecEstimate(uint64_t timestamp) const {
  if (timestamp < flow_->last_rx()) {
    throw std::logic_error("Cannot get a Bps estimate in the past");
  }

  const uint64_t curr_second_end = curr_second_start_ + kMillion;
  const double alpha = flow_->flow_config().tcp_estimator_ewma_alpha();

  double Bps =
      curr_second_start_ == flow_->first_rx() ?
          bytes_this_second_ : curr_bytes_per_second_;

  if (timestamp <= curr_second_end) {
    return Bps;
  }

  uint32_t seconds_skipped = (timestamp - curr_second_end) / kMillion;
  for (size_t i = 0; i < seconds_skipped; ++i) {
    Bps = (1 - alpha) * Bps;
  }

  return Bps;
}

void Flow::TCPIpRx(const pcap::SniffIp& ip_header,
                   const pcap::SniffTcp& tcp_header, uint64_t timestamp,
                   size_t* bytes) {
  size_t bytes_before = curr_size_bytes_;
  IpRx(ip_header, timestamp);

  uint32_t headers_size = (ip_header.ip_hl + tcp_header.th_off) * 4;
  uint32_t payload_size = ntohs(ip_header.ip_len) - headers_size;
  total_payload_seen_ += payload_size;
  uint32_t seq = ntohl(tcp_header.th_seq);

  if (flow_config_.fields_to_track_ & FlowConfig::HF_PAYLOAD_SIZE) {
    payload_size_.Append(payload_size, &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_TCP_FLAGS) {
    tcp_flags_.Append(tcp_header.th_flags, &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_TCP_SEQ) {
    tcp_seq_.Append(seq, &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_TCP_ACK) {
    tcp_ack_.Append(ntohl(tcp_header.th_ack), &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_TCP_WIN) {
    tcp_win_.Append(ntohs(tcp_header.th_win), &curr_size_bytes_);
  }

  tcp_rate_estimator_->UpdateEstimate(seq, payload_size, timestamp);
  last_rx_time_ = timestamp;
  *bytes += (curr_size_bytes_ - bytes_before);
}

void Flow::UDPIpRx(const pcap::SniffIp& ip_header,
                   const pcap::SniffUdp& udp_header, uint64_t timestamp,
                   size_t* bytes) {
  Unused(udp_header);
  size_t bytes_before = curr_size_bytes_;

  uint32_t headers_size = ip_header.ip_hl * 4 - pcap::kSizeUDP;
  uint32_t payload_size = ntohs(ip_header.ip_len) - headers_size;
  total_payload_seen_ += payload_size;
  if (flow_config_.fields_to_track_ & FlowConfig::HF_PAYLOAD_SIZE) {
    payload_size_.Append(payload_size, &curr_size_bytes_);
  }

  IpRx(ip_header, timestamp);
  last_rx_time_ = timestamp;
  *bytes += (curr_size_bytes_ - bytes_before);
}

void Flow::ICMPIpRx(const pcap::SniffIp& ip_header,
                    const pcap::SniffIcmp& icmp_header, uint64_t timestamp,
                    size_t* bytes) {
  size_t bytes_before = curr_size_bytes_;
  IpRx(ip_header, timestamp);

  uint32_t headers_size = ip_header.ip_hl * 4 - pcap::kSizeICMP;
  uint32_t payload_size = ntohs(ip_header.ip_len) - headers_size;
  total_payload_seen_ += payload_size;
  if (flow_config_.fields_to_track_ & FlowConfig::HF_PAYLOAD_SIZE) {
    payload_size_.Append(payload_size, &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_ICMP_TYPE) {
    icmp_type_.Append(icmp_header.icmp_type, &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_ICMP_CODE) {
    icmp_code_.Append(icmp_header.icmp_code, &curr_size_bytes_);
  }

  last_rx_time_ = timestamp;
  *bytes += (curr_size_bytes_ - bytes_before);
}

void Flow::UnknownIpRx(const pcap::SniffIp& ip_header, uint64_t timestamp,
                       size_t* bytes) {
  size_t bytes_before = curr_size_bytes_;
  IpRx(ip_header, timestamp);

  // This will be off, but we don't know what the protocol is.
  uint32_t payload_size = ntohs(ip_header.ip_len) - ip_header.ip_hl * 4;
  total_payload_seen_ += payload_size;
  if (flow_config_.fields_to_track_ & FlowConfig::HF_PAYLOAD_SIZE) {
    payload_size_.Append(payload_size, &curr_size_bytes_);
  }

  last_rx_time_ = timestamp;
  *bytes += (curr_size_bytes_ - bytes_before);
}

void Flow::IpRx(const pcap::SniffIp& ip_header, uint64_t timestamp) {
  if (state_ != FlowState::ACTIVE) {
    throw std::runtime_error("Tried to modify passive flow");
  }

  if (ip_header.ip_p != key_.protocol()) {
    throw std::runtime_error("Wrong proto type in PacketRx");
  }

  timestamps_.Append(timestamp, &curr_size_bytes_);

  uint16_t ip_len = ntohs(ip_header.ip_len);
  total_ip_len_seen_ += ip_len;

  if (flow_config_.fields_to_track_ & FlowConfig::HF_IP_LEN) {
    ip_len_.Append(ip_len, &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_IP_ID) {
    ip_id_.Append(ntohs(ip_header.ip_id), &curr_size_bytes_);
  }

  if (flow_config_.fields_to_track_ & FlowConfig::HF_IP_TTL) {
    ip_ttl_.Append(ip_header.ip_ttl, &curr_size_bytes_);
  }

  pkts_seen_++;
}

uint64_t TrackedFields::timestamp() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_TIMESTAMP)) {
    throw std::logic_error("timestamp not tracked");
  }

  return timestamp_;
}

uint16_t TrackedFields::ip_len() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_IP_LEN)) {
    throw std::logic_error("ip_len not tracked");
  }

  return ip_len_;
}

uint16_t TrackedFields::ip_id() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_IP_ID)) {
    throw std::logic_error("ip_id not tracked");
  }

  return ip_id_;
}

uint8_t TrackedFields::ip_ttl() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_IP_TTL)) {
    throw std::logic_error("ip_id not tracked");
  }

  return ip_ttl_;
}

uint32_t TrackedFields::tcp_seq() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_TCP_SEQ)) {
    throw std::logic_error("tcp_seq not tracked");
  }

  return tcp_seq_;
}

uint32_t TrackedFields::tcp_ack() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_TCP_ACK)) {
    throw std::logic_error("tcp_ack not tracked");
  }

  return tcp_ack_;
}

uint16_t TrackedFields::tcp_win() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_TCP_WIN)) {
    throw std::logic_error("tcp_win not tracked");
  }

  return tcp_win_;
}

uint8_t TrackedFields::tcp_flags() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_TCP_FLAGS)) {
    throw std::logic_error("tcp_flags not tracked");
  }

  return tcp_flags_;
}

uint16_t TrackedFields::payload_size() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_PAYLOAD_SIZE)) {
    throw std::logic_error("payload size not tracked");
  }

  return payload_size_;
}

uint8_t TrackedFields::icmp_code() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_ICMP_CODE)) {
    throw std::logic_error("icmp_code size not tracked");
  }

  return icmp_code_;
}

uint8_t TrackedFields::icmp_type() const {
  if (!(fields_present_bitmap_ & FlowConfig::HF_ICMP_TYPE)) {
    throw std::logic_error("icmp_type size not tracked");
  }

  return icmp_type_;
}

}
