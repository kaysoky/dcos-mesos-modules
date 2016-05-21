/**
 * This file is © 2014 Mesosphere, Inc. (“Mesosphere”). Mesosphere
 * licenses this file to you solely pursuant to the following terms
 * (and you may not use this file except in compliance with such
 * terms):
 *
 * 1) Subject to your compliance with the following terms, Mesosphere
 * hereby grants you a nonexclusive, limited, personal,
 * non-sublicensable, non-transferable, royalty-free license to use
 * this file solely for your internal business purposes.
 *
 * 2) You may not (and agree not to, and not to authorize or enable
 * others to), directly or indirectly:
 *   (a) copy, distribute, rent, lease, timeshare, operate a service
 *   bureau, or otherwise use for the benefit of a third party, this
 *   file; or
 *
 *   (b) remove any proprietary notices from this file.  Except as
 *   expressly set forth herein, as between you and Mesosphere,
 *   Mesosphere retains all right, title and interest in and to this
 *   file.
 *
 * 3) Unless required by applicable law or otherwise agreed to in
 * writing, Mesosphere provides this file on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
 * including, without limitation, any warranties or conditions of
 * TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * 4) In no event and under no legal theory, whether in tort (including
 * negligence), contract, or otherwise, unless required by applicable
 * law (such as deliberate and grossly negligent acts) or agreed to in
 * writing, shall Mesosphere be liable to you for damages, including
 * any direct, indirect, special, incidental, or consequential damages
 * of any character arising as a result of these terms or out of the
 * use or inability to use this file (including but not limited to
 * damages for loss of goodwill, work stoppage, computer failure or
 * malfunction, or any and all other commercial damages or losses),
 * even if Mesosphere has been advised of the possibility of such
 * damages.
 */

#include <stdio.h>

#include <list>

#include <stout/check.hpp>
#include <stout/interval.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/mac.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/module/anonymous.hpp>

#include <overlay/overlay.hpp>
#include <overlay/internal/messages.hpp>

namespace http = process::http;

using std::list;
using std::queue;
using std::string;
using std::vector;

using net::IP;
using net::IPNetwork;
using net::MAC;

using process::DESCRIPTION;
using process::HELP;
using process::Owned;
using process::Failure;
using process::Future;
using process::TLDR;
using process::UPID;
using process::USAGE;

using mesos::modules::Anonymous;
using mesos::modules::Module;
using mesos::modules::overlay::AgentOverlayInfo;
using mesos::modules::overlay::BackendInfo;
using mesos::modules::overlay::NetworkConfig;
using mesos::modules::overlay::VxLANInfo;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::UpdateAgentOverlaysMessage;
using mesos::Parameters;

namespace mesos {
namespace modules {
namespace overlay {
namespace master {

constexpr Duration PENDING_MESSAGE_PERIOD = Seconds(10);


const string OVERLAY_HELP = HELP(
    TLDR("Allocate overlay network resources for Master."),
    USAGE("/overlay-master/overlays"),
    DESCRIPTION("Allocate subnets, VTEP IP and the MAC addresses.", "")
);


struct Vtep
{
  Vtep(const IPNetwork& _network, const MAC _oui)
    : network(_network), oui(_oui)
  {
    uint32_t endIP = 0xffffffff >> (32 - network.prefix());
    uint32_t endMAC = 0xffffffff >> 8;

    freeIP += (Bound<uint32_t>::closed(1), Bound<uint32_t>::closed(endIP - 1));
    freeMAC += (Bound<uint32_t>::closed(1), Bound<uint32_t>::closed(endMAC - 1));
  }

  Try<IPNetwork> allocateIP()
  {
    if (freeIP.empty()) {
      return Error("Unable to allocate a VTEP IP due to exhaustion");
    }

    uint32_t ip = freeIP.begin()->lower();
    freeIP -= ip;

    uint32_t address = ntohl(network.address().in().get().s_addr);
    address += ip;

    return IPNetwork::create(net::IP(address),network.prefix()) ;
  }

  Try<Nothing> deAllocate(const IPNetwork& _network)
  {
    if (_network.prefix() != network.prefix()) {
      return Error(
          "Cannot free this network because prefix " +
          stringify(_network.prefix()) + " does not match Agent prefix "
          + stringify(network.prefix()) + " of the overlay");
    }

    if (_network.prefix() < network.prefix()) {
      return Error(
          "Cannot free this network since it does not belong "
          " to the overlay subnet");
    }

    uint32_t address = ntohl(_network.address().in().get().s_addr);
    uint32_t mask = ntohl(_network.netmask().in().get().s_addr);

    address &= ~mask;

    freeIP += address;

    return Nothing();
  }

  Try<MAC> allocateMAC()
  {
    if (freeMAC.empty()) {
      return Error("Unable to allocate VTEP MAC due to exhaustion");
    }

    uint32_t _nic = freeMAC.begin()->lower();
    freeMAC -= _nic;

    _nic = htonl(_nic);

    uint8_t* nic = (uint8_t*)&_nic;

    uint8_t mac[6];

    //Set the OUI.
    mac[0] = oui[0];
    mac[1] = oui[1];
    mac[2] = oui[2];

    //Set the NIC.
    mac[3] = nic[1];
    mac[4] = nic[2];
    mac[5] = nic[3];

    return MAC(mac);
  }

  Try<Nothing> deAllocate(const MAC& mac)
  {
    if (mac[0] != oui[0] || mac[1] != oui[1] || mac[2] != oui[2]) {
      return Error("Unable to free MAC for an unknown OUI");
    }

    uint32_t _nic ;

    uint8_t* nic = (uint8_t*) &_nic;
    nic[1] = mac[3];
    nic[2] = mac[4];
    nic[3] = mac[5];

    _nic = ntohl(_nic);

    freeMAC += _nic;

    return Nothing();
  }

  // Network allocated to the VTEP.
  IPNetwork network;

  MAC oui;

  IntervalSet<uint32_t> freeIP;
  IntervalSet<uint32_t> freeMAC;
};


struct Overlay
{
  Overlay(
      const string& _name,
      const net::IPNetwork& _network,
      const uint8_t _prefix)
    : name(_name),
    network(_network),
    prefix(_prefix)
  {
    // `network` has already been vetted to be an AF_INET address.
    uint32_t endSubnet = 0xffffffff; // 255.255.255.255
    endSubnet = endSubnet >> (network.prefix() + 32 - prefix);

    freeNetworks +=
      (Bound<uint32_t>::closed(0),
       Bound<uint32_t>::closed(endSubnet));
  }

  OverlayInfo getOverlayInfo() const 
  {
    OverlayInfo overlay;

    overlay.set_name(name);
    overlay.set_subnet(stringify(network));
    overlay.set_prefix(prefix);

    return overlay;
  }

  Try<net::IPNetwork> allocate()
  {
    if (freeNetworks.empty()) {
      return Error("No free subnets available in the " + name + "overlay");
    }

    uint32_t subnet = freeNetworks.begin()->lower();
    freeNetworks -= subnet;

    uint32_t agentSubnet = ntohl(network.address().in().get().s_addr);

    // Get the suffix of the Agent subnet.
    subnet = subnet << (32 - prefix);
    agentSubnet |= subnet;

    return net::IPNetwork::create(net::IP(agentSubnet), prefix);
  }

  Try<Nothing> free(const net::IPNetwork& subnet)
  {
    if (subnet.prefix() != prefix) {
      return Error(
          "Cannot free this network because prefix " + stringify(subnet.prefix()) +
          " does not match Agent prefix " + stringify(prefix) + " of the overlay");
    }

    if (subnet.prefix() < network.prefix()) {
      return Error(
          "Cannot free this network since it does not belong "
          " to the overlay subnet");
    }

    uint32_t address = ntohl(subnet.address().in().get().s_addr);

    freeNetworks += address;

    return Nothing();
  }

  // Canonical name of the network.
  std::string name;

  // Network allocated to this overlay.
  net::IPNetwork network;

  // Prefix length allocated to each agent.
  uint8_t prefix;

  // Free subnets available in this network. The subnets are
  // calcualted using the prefix length set for the agents in
  // `prefix`.
  IntervalSet<uint32_t> freeNetworks;
};


class Agent
{
public:
  Agent(const UPID& _pid) : pid(_pid) {};

  void addOverlay(const AgentOverlayInfo& overlay)
  {
    if (overlays.contains(overlay.info().name())) {
      return;
    }

    overlays[overlay.info().name()].CopyFrom(overlay);
  }

  list<AgentOverlayInfo> getOverlays() const
  {
    list<AgentOverlayInfo> _overlays;

    foreachvalue (const AgentOverlayInfo& overlay, overlays) {
      _overlays.push_back(overlay);
    }

    return _overlays;
  }

  void clearOverlaysState()
  {
    foreachvalue (AgentOverlayInfo& overlay, overlays) {
      overlay.clear_state();
    }
  }

  AgentInfo getAgentInfo() const
  {
    AgentInfo info;

    info.set_ip(stringify(pid.address.ip));

    foreachvalue(const AgentOverlayInfo& overlay, overlays) {
      info.add_overlays()->CopyFrom(overlay);
    }

    return info;
  }

  void updateOverlayState(const AgentOverlayInfo& overlay)
  {
    const string name = overlay.info().name();
    if (!overlays.contains(name)) {
      LOG(ERROR) << "Got update for unknown network "
                 << overlay.info().name() ;
    }

    overlays[name].mutable_state()->set_status(overlay.state().status());
  }

private:
  const UPID pid;

  // A list of all overlay networks that reside on this agent.
  hashmap<string, AgentOverlayInfo> overlays;
};


// `ManagerProcess` is responsible for managing all the overlays that
// exist in the Mesos cluster. For each overlay the manager stores the
// network associated with overlay and the prefix length of subnets
// that need to be assigned to Agents. When an Agent registers with
// the manager, the manager picks a network from each overlay that the
// manager is aware off and assigns it to the Agent. When the Agent
// de-registers (or goes away) the manager frees subnets allocated to
// the Agent for all the overlays that existed on that Agent.
class ManagerProcess : public ProtobufProcess<ManagerProcess>
{
public:
  static Try<Owned<ManagerProcess>> createManagerProcess(
      const NetworkConfig& networkConfig)
  {
    Try<net::IPNetwork> vtepSubnet =
      net::IPNetwork::parse(networkConfig.vtep_subnet(), AF_INET);
    if (vtepSubnet.isError()) {
      return Error(
          "Unable to parse the VTEP Subnet: " + vtepSubnet.error());
    }

    vector<string> tokens = strings::split(networkConfig.vtep_mac_oui(), ":");
    if (tokens.size() != 6) {
      return Error(
          "Invalid OUI MAC address. Mac address " +
          networkConfig.vtep_mac_oui() + " needs to be in"
          " the format xx:xx:xx:00:00:00");
    }

    uint8_t mac[6];
    for (size_t i = 0; i < tokens.size(); i++) {
      sscanf(tokens[i].c_str(), "%hhx", &mac[i]);
      if ( i > 2 && mac[i] != 0) {
        return Error(
          "Invalid OUI MAC address: " + networkConfig.vtep_mac_oui() +
          ". Least significant three bytes should not be"
          " set for the OUI"); 
        
      }
    }

    net::MAC vtepMACOUI(mac);

    hashmap<string, Overlay> overlays;
    IntervalSet<uint32_t> addressSpace;

    // Overlay networks cannot have overlapping IP addresses. This
    // lambda keeps track of the current address space and returns an
    // `Error` if it detects an overlay that is going to use an
    // already configured address space.
    auto updateAddressSpace =
      [&addressSpace](const IPNetwork &network) -> Try<Nothing> {
        uint32_t startIP = ntohl(network.address().in().get().s_addr);

        uint32_t mask = ntohl(network.netmask().in().get().s_addr);
        mask = ~mask;

        uint32_t endIP = startIP | mask;

        Interval<uint32_t> overlaySpace =
          (Bound<uint32_t>::closed(startIP), Bound<uint32_t> ::closed(endIP));

        if (addressSpace.intersects(overlaySpace)) {
          return Error("Found overlapping address spaces");
        }

        addressSpace += overlaySpace;

        return Nothing();
      };

    for (int i = 0; i < networkConfig.overlays_size(); i++) {
      OverlayInfo overlay = networkConfig.overlays(i);

      if (overlays.contains(overlay.name())) {
        return Error(
            "Duplicate overlay configuration detected for overlay: " +
            overlay.name());
      }

      LOG(INFO) << "Configuring overlay network:" << overlay.name();

      Try<net::IPNetwork> address =
        net::IPNetwork::parse(overlay.subnet(), AF_INET);
      if (address.isError()) {
        return Error(
            "Unable to determine subnet for network: " +
            stringify(address.get()));
      }

      Try<Nothing> valid = updateAddressSpace(address.get());

      if (valid.isError()) {
        return Error(
            "Incorrect address space for the overlay network '" +
            overlay.name() + "': " + valid.error());
      }

      overlays.emplace(
          overlay.name(),
          Overlay(
            overlay.name(),
            address.get(),
            (uint8_t) overlay.prefix()));
    }

    if (overlays.empty()) {
      return Error(
          "Could not find any overlay configuration. Specify at"
          " least one overlay");
    }

    return Owned<ManagerProcess>(
        new ManagerProcess(
          overlays,
          vtepSubnet.get(),
          vtepMACOUI));

  }

protected:
  virtual void initialize()
  {
    LOG(INFO) << "Adding route for '" << self().id << "/state'";

    route("/state",
          OVERLAY_HELP,
          &ManagerProcess::state);

    // When a new agent comes up or an existing agent reconnects with
    // the master, it'll first send a `RegisterAgentMessage` to the
    // master. The master will reply with `UpdateAgentNetworkMessage`.
    install<RegisterAgentMessage>(&ManagerProcess::registerAgent);

    // When the agent finishes its configuration based on the content
    // in `UpdateAgentNetworkMessage`, it'll reply the master with an
    // `AgentRegisteredMessage`.
    // TODO(jieyu): Master should retry `UpdateAgentNetworkMessage` in
    // case the message gets dropped.
    install<AgentRegisteredMessage>(&ManagerProcess::agentRegistered);
  }

  void registerAgent(const UPID& pid)
  {
    list<AgentOverlayInfo> _overlays;

    if (!std::get<1>(agents.emplace(pid, pid))) {
      LOG(INFO) << "Agent " << pid << "re-registering";

      // Reset existing state of the overlays, since the Agent, after
      // restart, does not expect the overlays to have any state.
      agents.at(pid).clearOverlaysState();

      _overlays = agents.at(pid).getOverlays();
    } else {
      LOG(INFO) << "Got registration from pid: " << pid;

      Try<net::IPNetwork> vtepIP = vtep.allocateIP();
      if (vtepIP.isError()) {
        LOG(ERROR) << "Unable to get VTEP IP for Agent: " << vtepIP.error();
      }

      Try<net::MAC> vtepMAC = vtep.allocateMAC();
      if (vtepMAC.isError()) {
        LOG(ERROR) << "Unable to get VTEP MAC for Agent: " << vtepMAC.error();
      }

      // Walk through all the overlay networks. Allocate a subnet from
      // each overlay to the Agent. Allocate a VTEP IP and MAC for each
      // agent. Queue the message on the Agent. Finally, ask Master to
      // reliably send these messages to the Agent.
      foreachpair (const string& name, Overlay& overlay, overlays) {
        AgentOverlayInfo _overlay;

        _overlay.mutable_info()->set_name(name);
        _overlay.mutable_info()->set_subnet(stringify(overlay.network));
        _overlay.mutable_info()->set_prefix(overlay.prefix);

        Try<net::IPNetwork> agentSubnet = overlay.allocate();
        if (agentSubnet.isError()) {
          LOG(ERROR) << "Cannot allocate subnet from overlay "
                     << name << " to Agent " << pid;
          continue;
        }
        _overlay.set_subnet(stringify(agentSubnet.get()));

        // Allocate bridges for CNI and Docker.
        Try<Nothing> bridges = allocateBridges(_overlay);

        if (bridges.isError()) {
          LOG(ERROR) << "Unable to allocate bridge for network "
                     << name << ": " << bridges.error();
          overlay.free(agentSubnet.get());
          continue;
        }

        VxLANInfo vxlan;
        vxlan.set_vni(1024);
        vxlan.set_vtep_name("vtep1024");
        vxlan.set_vtep_ip(stringify(vtepIP.get()));
        vxlan.set_vtep_mac(stringify(vtepMAC.get()));

        BackendInfo backend;
        backend.mutable_vxlan()->CopyFrom(vxlan);

        _overlay.mutable_backend()->CopyFrom(backend);

        agents.at(pid).addOverlay(_overlay);
      }

      _overlays = agents.at(pid).getOverlays();
    }

    // Create the network update message and send it to the Agent.
    UpdateAgentOverlaysMessage update;

    foreach(const AgentOverlayInfo& overlay, _overlays) {
      update.add_overlays()->CopyFrom(overlay);
    }

    send(pid, update);
  }

  void agentRegistered(const UPID& from, const AgentRegisteredMessage& message)
  {
    if(agents.contains(from)) {
      LOG(INFO) << "Got ACK for addition of networks from " << from;
      for(int i=0; i < message.overlays_size(); i++) {
        agents.at(from).updateOverlayState(message.overlays(i));
      }

      send(from, AgentRegisteredAcknowledgement());
    } else {
      LOG(ERROR) << "Got ACK for network message for non-existent PID "
        << from;
    }
  }

  Try<Nothing> allocateBridges(AgentOverlayInfo& _overlay)
  {
    const string name = _overlay.info().name();

    Try<IPNetwork> network = net::IPNetwork::parse(
        _overlay.subnet(),
        AF_INET);

    if (network.isError()) {
      return Error("Unable to parse the subnet of the network '" +
          name + "' : " + network.error());
    }

    Try<struct in_addr> subnet = network->address().in();
    Try<struct in_addr> subnetMask = network->netmask().in();

    if (subnet.isError()) {
      return Error("Unable to get a 'struct in_addr' representation of "
          "the network :"  + subnet.error());
    }

    if (subnetMask.isError()) {
      return Error("Unable to get a 'struct in_addr' representation of "
          "the mask :"  + subnetMask.error());
    }

    uint32_t address = ntohl(subnet->s_addr);
    uint32_t mask = ntohl(subnetMask->s_addr) |
      (0x1 << (32 - (network->prefix() + 1)));

    // Create the CNI bridge.
    Try<IPNetwork> cniSubnet = net::IPNetwork::create((IP(address)), (IP(mask)));

    if (cniSubnet.isError()) {
      return Error(
          "Could not create Mesos subnet for network '" +
          name + "': " + cniSubnet.error());
    }

    // Create the docker bridge.
    Try<IPNetwork> dockerSubnet = net::IPNetwork::create(
        IP(address | (0x1 << (32 - (network->prefix() + 1)))),
        IP(mask));

    if (dockerSubnet.isError()) {
      return Error(
          "Could not create Docker subnet for network '" +
          name + "': " + dockerSubnet.error());
    }

    //Update the bridge info.
    BridgeInfo cniBridgeInfo;
    cniBridgeInfo.set_ip(stringify(cniSubnet.get()));
    cniBridgeInfo.set_name(CNI_BRIDGE_PREFIX + name);
    _overlay.mutable_mesos_bridge()->CopyFrom(cniBridgeInfo);

    BridgeInfo dockerBridgeInfo;
    dockerBridgeInfo.set_ip(stringify(dockerSubnet.get()));
    dockerBridgeInfo.set_name(DOCKER_BRIDGE_PREFIX + name);
    _overlay.mutable_docker_bridge()->CopyFrom(dockerBridgeInfo);

    return Nothing();
  }

  Future<http::Response> state(const http::Request& request)
  {
    State state;

    VLOG(1) << "Responding to `state` endpoint";

    foreachvalue (const Overlay& overlay, overlays) {
      state.add_overlays()->CopyFrom(overlay.getOverlayInfo());
    }

    foreachvalue (const Agent& agent, agents) {
      state.add_agents()->CopyFrom(agent.getAgentInfo());
    }

    return http::OK(
        JSON::protobuf(state),
        request.url.query.get("jsonp"));
  }

private:
  ManagerProcess(
      const hashmap<string, Overlay>& _overlays,
      const net::IPNetwork& vtepSubnet,
      const net::MAC& vtepMACOUI)
    : ProcessBase("overlay-master"),
      overlays(_overlays),
      vtep(vtepSubnet, vtepMACOUI) {};

  hashmap<string, Overlay> overlays;
  hashmap<UPID, Agent> agents;

  Vtep vtep;
};

class Manager : public Anonymous
{
public:
  static Try<Manager*> createManager(const NetworkConfig& networkConfig)
  {
    Try<Owned<ManagerProcess>> process =
      ManagerProcess::createManagerProcess(networkConfig);

    if (process.isError()) {
      return Error(
          "Unable to create the `Manager` process: " +
          process.error());
    }

    return new Manager(process.get());
  }

  virtual ~Manager()
  {
    VLOG(1) << "Terminating process";

    terminate(process.get());
    wait(process.get());
  }

private:
  Manager(Owned<ManagerProcess> _process)
  : process(_process)
  {
    VLOG(1) << "Spawning process";

    spawn(process.get());
  }

  Owned<ManagerProcess> process;
};

} // namespace mesos {
} // namespace modules {
} // namespace overlay {
} // namespace master {

using mesos::modules::overlay::master::Manager;
using mesos::modules::overlay::master::ManagerProcess;

Try<NetworkConfig> parseNetworkConfig(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<NetworkConfig> parse =
    ::protobuf::parse<NetworkConfig>(json.get());

  if (parse.isError()) {
    return Error("Protobuf parse failed: " + parse.error());
  }

  return parse.get();
}

// Module "main".
Anonymous* createOverlayMasterManager(const Parameters& parameters)
{
  Option<NetworkConfig> networkConfig = None();

  VLOG(1) << "Parameters:";
  foreach (const mesos::Parameter& parameter, parameters.parameter()) {
    VLOG(1) << parameter.key() << ": " << parameter.value();

    if (parameter.key() == "network_config") {
      if (!os::exists(parameter.value())) {
        EXIT(EXIT_FAILURE)
          << "Unable to find the network configuration";
      }

      Try<string> config = os::read(parameter.value());
      if (config.isError()) {
        EXIT(EXIT_FAILURE) << "Unable to read the network configuration: "
                           << config.error();
      }

      Try<NetworkConfig> _networkConfig = parseNetworkConfig(config.get());

      if (_networkConfig.isError()) {
        EXIT(EXIT_FAILURE)
          << "Unable to prase the overlay JSON configuration: "
          << _networkConfig.error();
      }

      networkConfig = _networkConfig.get();
    }
  }

  if (networkConfig.isNone()) {
    EXIT(EXIT_FAILURE) << "No network configuration specified";
  }

  Try<Manager*> manager = Manager::createManager(networkConfig.get());

  if (manager.isError()) {
    EXIT(EXIT_FAILURE)
      << "Unable to create the Master manager module: "
      << manager.error();

  }

  return manager.get();
}


// Declares a helper module named 'Manager'.
Module<Anonymous> com_mesosphere_mesos_OverlayMasterManager(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "kapil@mesosphere.io",
    "Master Overlay Helper Module.",
    NULL,
    createOverlayMasterManager);
