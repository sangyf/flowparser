// Defines the main parser class.

#ifndef FLOWPARSER_PARSER_H
#define FLOWPARSER_PARSER_H

#include <functional>
#include <memory>
#include <unordered_map>

#include "common.h"
#include "sniff.h"
#include "flows.h"

namespace flowparser {

// Each flow is indexed by this value. Note that it does not contain a flow
// type. Only two flow types are supported - TCP and UDP and each has a
// separate map.
class FlowKey {
 public:
  FlowKey(const pcap::SniffIp ip_header, const pcap::SniffTcp tcp_header)
      : src_(ip_header.ip_src.s_addr),
        dst_(ip_header.ip_dst.s_addr),
        sport_(tcp_header.th_sport),
        dport_(tcp_header.th_dport) {
  }

  FlowKey(const pcap::SniffIp ip_header, const pcap::SniffUdp udp_header)
      : src_(ip_header.ip_src.s_addr),
        dst_(ip_header.ip_dst.s_addr),
        sport_(udp_header.uh_sport),
        dport_(udp_header.uh_dport) {
  }

  bool operator==(const FlowKey &other) const {
    return (src_ == other.src_ && dst_ == other.dst_ && sport_ == other.sport_
        && dport_ == other.dport_);
  }

  // The source IP address of the flow (in host byte order)
  uint32_t src() const {
    return ntohl(src_);
  }

  // The destination IP address of the flow (in host byte order)
  uint32_t dst() const {
    return ntohl(dst_);
  }

  // The source port of the flow (in host byte order)
  uint16_t src_port() const {
    return ntohs(sport_);
  }

  // The destination port of the flow (in host byte order)
  uint16_t dst_port() const {
    return ntohs(dport_);
  }

  size_t hash() const {
    size_t result = 17;
    result = 37 * result + src_;
    result = 37 * result + dst_;
    result = 37 * result + sport_;
    result = 37 * result + dport_;
    return result;
  }

 private:
  const uint32_t src_;
  const uint32_t dst_;
  const uint16_t sport_;
  const uint16_t dport_;
};

struct KeyHasher {
  size_t operator()(const FlowKey& k) const {
    return k.hash();
  }
};

// The main parser class. This class stores tables with flow data and owns all
// flow instances.
class FlowParser {
 public:
  FlowParser(std::function<void(std::unique_ptr<TCPFlow>)> callback,
             uint64_t timeout)
      : flow_timeout_(timeout),
        last_rx_(std::numeric_limits<uint64_t>::max()),
        callback_(callback) {
  }

  // Called when a new TCP packet arrives.
  Status HandlePkt(const pcap::SniffIp& ip_header,
                   const pcap::SniffTcp& transport_header, uint64_t timestamp);

  // Times out flows that have expired.
  void CollectFlows();

 private:
  typedef std::pair<std::mutex, std::unique_ptr<TCPFlow>> FlowValue;

  // How long to wait before collecting flows. This is not in real time, but in
  // time measured as per pcap timestamps. This means that "time" has whatever
  // precision the pcap timestamps give (usually microseconds) and only advances
  // when packets are received.
  const uint64_t flow_timeout_;

  // Last time a packet was received.
  uint64_t last_rx_;

  // A map to store TCP flows.
  std::unordered_map<FlowKey, FlowValue, KeyHasher> flows_table_;

  // A mutex for the flows table.
  std::mutex flows_table_mutex_;

  // When a TCP flow is complete it gets handed to this callback.
  const std::function<void(std::unique_ptr<TCPFlow>)> callback_;

  DISALLOW_COPY_AND_ASSIGN(FlowParser);
};

}

#endif  /* FLOWPARSER_PARSER_H */
