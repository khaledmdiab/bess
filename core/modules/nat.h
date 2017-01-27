#ifndef BESS_MODULES_NAT_H_
#define BESS_MODULES_NAT_H_

#include <arpa/inet.h>
#include <rte_config.h>
#include <rte_hash_crc.h>

#include <map>
#include <utility>
#include <vector>
#include <tuple>

#include "../module.h"
#include "../module_msg.pb.h"
#include "../utils/htable.h"
#include "../utils/ip.h"
#include "../utils/random.h"

using bess::utils::IPAddress;
using bess::utils::CIDRNetwork;
using bess::utils::HTable;

const uint16_t MIN_PORT = 1024;
const uint16_t MAX_PORT = 65535;
const uint64_t TIME_OUT_NS = 120L * 1000 * 1000 * 1000;

// 5 tuple for TCP/UDP packets with an additional icmp_ident for ICMP query pkts
class Flow {
 public:
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t icmp_ident;  // identifier of ICMP query
  uint8_t proto;

  Flow()
      : src_ip(0),
        dst_ip(0),
        src_port(0),
        dst_port(0),
        icmp_ident(0),
        proto(0) {}

  Flow(uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint16_t ident,
       uint8_t protocol)
      : src_ip(sip),
        dst_ip(dip),
        src_port(sp),
        dst_port(dp),
        icmp_ident(ident),
        proto(protocol) {}

  // Returns a new instance of reserse flow
  Flow ReverseFlow() const {
    return Flow(dst_ip, src_ip, dst_port, src_port, icmp_ident, proto);
  }

  bool operator!=(const Flow &other) const {
    return proto != other.proto || src_ip != other.src_ip ||
           src_port != other.src_port || dst_ip != other.dst_ip ||
           dst_port != other.dst_port || icmp_ident != other.icmp_ident;
  }
};


// Stores flow information
class FlowRecord {
 public:
  uint16_t port;
  Flow internal_flow;
  Flow external_flow;
  uint64_t time;

  FlowRecord() : port(), internal_flow(), external_flow(), time() {}
};

// A data structure to track available ports for a given subnet of external IPs
// for the NAT.  Encapsulates the subnet and the mapping from IPs in that subnet
// to free ports.
class AvailablePorts {
 public:
  // Tracks available ports within the given IP prefix.
  explicit AvailablePorts(const CIDRNetwork &prefix) : prefix_(prefix), free_list_(), next_expiry_() {
    uint32_t min = ntohl(prefix_.addr & prefix_.mask);
    uint32_t max = ntohl(prefix_.addr | (~prefix_.mask));

    for (uint32_t ip = min; ip <= max; ip++) {
      for (uint32_t port = MIN_PORT; port <= MAX_PORT; port++) {
        free_list_.emplace_back(htonl(ip), htons((uint16_t) port), new FlowRecord());
      }
    }
    std::random_shuffle(free_list_.begin(), free_list_.end());
  }

  ~AvailablePorts() {
    for (auto &i : free_list_) {
      FlowRecord *r = std::get<2>(i);
      delete r;
    }
  }

  // Returns a random free IP/port pair within the network and removes it from
  // the free list.
  std::tuple<IPAddress, uint16_t, FlowRecord *> RandomFreeIPAndPort() {
    std::tuple<IPAddress, uint16_t, FlowRecord *> r = free_list_.back();
    free_list_.pop_back();
    return r;
  }

  // Adds the given IP/port pair back to the list of available ports.  Takes
  // back ownership of the tuple and the FlowRecord object in it.
  void FreeAllocated(const std::tuple<IPAddress, uint16_t, FlowRecord *> &a) {
    free_list_.push_back(a);
  }

  // Returns true if there are no free remaining IP/port pairs.
  bool Empty() const {
    return free_list_.empty();
  }

  const CIDRNetwork &prefix() const { return prefix_; }

  uint64_t next_expiry() const { return next_expiry_; }

  void set_next_expiry(uint64_t next_expiry) { next_expiry_ = next_expiry; }

 private:
  CIDRNetwork prefix_;
  std::vector<std::tuple<IPAddress, uint16_t, FlowRecord *>> free_list_;
  uint64_t next_expiry_;
};

struct FlowHash {
  std::size_t operator()(const Flow &f) const {
    static_assert(sizeof(Flow) == 2 * sizeof(uint64_t),
                  "Flow must be 16 bytes.");
    const uint64_t *flowdata = reinterpret_cast<const uint64_t *>(&f);
    uint32_t init_val = 0;
#if __SSE4_2__ && __x86_64
    init_val = crc32c_sse42_u64(*flowdata++, init_val);
    init_val = crc32c_sse42_u64(*flowdata++, init_val);
#else
    init_val = rte_hash_crc(flowdata, sizeof(Flow), init_val);
#endif
    return init_val;
  }
};

// NAT module. 2 igates and 2 ogates
// igate/ogate 0: traffic from internal network to external network
// igate/ogate 1: traffic from external network to internal network
class NAT final : public Module {
 public:
  static const Commands cmds;
  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 2;

  pb_error_t Init(const bess::pb::NATArg &arg);
  void DeInit() override;

  void ProcessBatch(bess::PacketBatch *batch) override;

  pb_cmd_response_t CommandAdd(const bess::pb::NATArg &arg);
  pb_cmd_response_t CommandClear(const bess::pb::EmptyArg &arg);

 private:
  void InitRules(const bess::pb::NATArg &arg) {
    for (const auto &rule : arg.rules()) {
      CIDRNetwork int_net(rule.internal_addr_block());
      CIDRNetwork ext_net(rule.external_addr_block());
      rules_.emplace_back(std::piecewise_construct,
                          std::forward_as_tuple(int_net),
                          std::forward_as_tuple(ext_net));
    }
  }

  static inline int flow_keycmp(const void *key, const void *key_stored,
                                size_t) {
    return *(const Flow *)key != *(const Flow *)key_stored;
  }

  static inline uint32_t flow_hash(const void *key, uint32_t,
                                   uint32_t init_val) {
#if __SSE4_2__ && __x86_64
    const uint64_t *flowdata = reinterpret_cast<const uint64_t *>(key);
    init_val = crc32c_sse42_u64(*flowdata++, init_val);
    init_val = crc32c_sse42_u64(*flowdata, init_val);
#else
    init_val = rte_hash_crc(key, sizeof(Flow), init_val);
#endif
    return init_val;
  }

  std::vector<std::pair<CIDRNetwork, AvailablePorts>> rules_;
  HTable<Flow, FlowRecord *, flow_keycmp, flow_hash> flow_hash_;
  Random rng_;
};

#endif  // BESS_MODULES_NAT_H_