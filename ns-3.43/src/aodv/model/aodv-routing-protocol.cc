/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Authors: Elena Buchatskaia <borovkovaes@iitp.ru>
 * Pavel Boyko <boyko@iitp.ru>
 * Modified by: User for AODV-EOCW Fuzzy Implementation
 */

#include "aodv-routing-protocol.h"

#include "ns3/adhoc-wifi-mac.h"
#include "ns3/boolean.h"
#include "ns3/inet-socket-address.h"
#include "ns3/log.h"
#include "ns3/pointer.h"
#include "ns3/random-variable-stream.h"
#include "ns3/string.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-net-device.h"
#include "ns3/node.h"
#include "ns3/energy-source-container.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-mac-queue.h"

#include <algorithm>
#include <limits>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("AodvRoutingProtocol");

namespace aodv
{
NS_OBJECT_ENSURE_REGISTERED(RoutingProtocol);

const uint32_t RoutingProtocol::AODV_PORT = 654;

class DeferredRouteOutputTag : public Tag
{
  public:
    DeferredRouteOutputTag(int32_t o = -1) : Tag(), m_oif(o) {}

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::aodv::DeferredRouteOutputTag")
                                .SetParent<Tag>()
                                .SetGroupName("Aodv")
                                .AddConstructor<DeferredRouteOutputTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    int32_t GetInterface() const { return m_oif; }
    void SetInterface(int32_t oif) { m_oif = oif; }
    uint32_t GetSerializedSize() const override { return sizeof(int32_t); }
    void Serialize(TagBuffer i) const override { i.WriteU32(m_oif); }
    void Deserialize(TagBuffer i) override { m_oif = i.ReadU32(); }
    void Print(std::ostream& os) const override { os << "DeferredRouteOutputTag: output interface = " << m_oif; }

  private:
    int32_t m_oif;
};

NS_OBJECT_ENSURE_REGISTERED(DeferredRouteOutputTag);

//-----------------------------------------------------------------------------
RoutingProtocol::RoutingProtocol()
    : m_rreqRetries(2),
      m_ttlStart(1),
      m_ttlIncrement(2),
      m_ttlThreshold(7),
      m_timeoutBuffer(2),
      m_rreqRateLimit(10),
      m_rerrRateLimit(10),
      m_activeRouteTimeout(Seconds(3)),
      m_netDiameter(35),
      m_nodeTraversalTime(MilliSeconds(40)),
      m_netTraversalTime(Time((2 * m_netDiameter) * m_nodeTraversalTime)),
      m_pathDiscoveryTime(Time(2 * m_netTraversalTime)),
      m_myRouteTimeout(Time(2 * std::max(m_pathDiscoveryTime, m_activeRouteTimeout))),
      m_helloInterval(Seconds(1)),
      m_allowedHelloLoss(2),
      m_deletePeriod(Time(5 * std::max(m_activeRouteTimeout, m_helloInterval))),
      m_nextHopWait(m_nodeTraversalTime + MilliSeconds(10)),
      m_blackListTimeout(Time(m_rreqRetries * m_netTraversalTime)),
      m_maxQueueLen(64),
      m_maxQueueTime(Seconds(30)),
      m_destinationOnly(false),
      m_gratuitousReply(true),
      m_enableHello(false),
      m_enableFuzzy(true), // Default True
      m_routingTable(m_deletePeriod),
      m_queue(m_maxQueueLen, m_maxQueueTime),
      m_requestId(0),
      m_seqNo(0),
      m_rreqIdCache(m_pathDiscoveryTime),
      m_dpd(m_pathDiscoveryTime),
      m_nb(m_helloInterval),
      m_rreqCount(0),
      m_rerrCount(0),
      m_htimer(Timer::CANCEL_ON_DESTROY),
      m_rreqRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_rerrRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_lastBcastTime(Seconds(0))
{
    m_nb.SetCallback(MakeCallback(&RoutingProtocol::SendRerrWhenBreaksLinkToNextHop, this));
}

TypeId
RoutingProtocol::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::aodv::RoutingProtocol")
            .SetParent<Ipv4RoutingProtocol>()
            .SetGroupName("Aodv")
            .AddConstructor<RoutingProtocol>()
            .AddAttribute("HelloInterval", "HELLO messages emission interval.", TimeValue(Seconds(1)), MakeTimeAccessor(&RoutingProtocol::m_helloInterval), MakeTimeChecker())
            .AddAttribute("TtlStart", "Initial TTL value for RREQ.", UintegerValue(1), MakeUintegerAccessor(&RoutingProtocol::m_ttlStart), MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlIncrement", "TTL increment for each attempt using the expanding ring search for RREQ dissemination.", UintegerValue(2), MakeUintegerAccessor(&RoutingProtocol::m_ttlIncrement), MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlThreshold", "Maximum TTL value for expanding ring search, TTL = NetDiameter is used beyond this value.", UintegerValue(7), MakeUintegerAccessor(&RoutingProtocol::m_ttlThreshold), MakeUintegerChecker<uint16_t>())
            .AddAttribute("TimeoutBuffer", "Provide a buffer for the timeout.", UintegerValue(2), MakeUintegerAccessor(&RoutingProtocol::m_timeoutBuffer), MakeUintegerChecker<uint16_t>())
            .AddAttribute("RreqRetries", "Maximum number of retransmissions of RREQ to discover a route", UintegerValue(2), MakeUintegerAccessor(&RoutingProtocol::m_rreqRetries), MakeUintegerChecker<uint32_t>())
            .AddAttribute("RreqRateLimit", "Maximum number of RREQ per second.", UintegerValue(10), MakeUintegerAccessor(&RoutingProtocol::m_rreqRateLimit), MakeUintegerChecker<uint32_t>())
            .AddAttribute("RerrRateLimit", "Maximum number of RERR per second.", UintegerValue(10), MakeUintegerAccessor(&RoutingProtocol::m_rerrRateLimit), MakeUintegerChecker<uint32_t>())
            .AddAttribute("NodeTraversalTime", "Conservative estimate of the average one hop traversal time for packets and should include queuing delays, interrupt processing times and transfer times.", TimeValue(MilliSeconds(40)), MakeTimeAccessor(&RoutingProtocol::m_nodeTraversalTime), MakeTimeChecker())
            .AddAttribute("NextHopWait", "Period of our waiting for the neighbour's RREP_ACK = 10 ms + NodeTraversalTime", TimeValue(MilliSeconds(50)), MakeTimeAccessor(&RoutingProtocol::m_nextHopWait), MakeTimeChecker())
            .AddAttribute("ActiveRouteTimeout", "Period of time during which the route is considered to be valid", TimeValue(Seconds(3)), MakeTimeAccessor(&RoutingProtocol::m_activeRouteTimeout), MakeTimeChecker())
            .AddAttribute("MyRouteTimeout", "Value of lifetime field in RREP generating by this node = 2 * max(ActiveRouteTimeout, PathDiscoveryTime)", TimeValue(Seconds(11.2)), MakeTimeAccessor(&RoutingProtocol::m_myRouteTimeout), MakeTimeChecker())
            .AddAttribute("BlackListTimeout", "Time for which the node is put into the blacklist = RreqRetries * NetTraversalTime", TimeValue(Seconds(5.6)), MakeTimeAccessor(&RoutingProtocol::m_blackListTimeout), MakeTimeChecker())
            .AddAttribute("DeletePeriod", "DeletePeriod is intended to provide an upper bound on the time for which an upstream node A can have a neighbor B as an active next hop for destination D, while B has invalidated the route to D. = 5 * max (HelloInterval, ActiveRouteTimeout)", TimeValue(Seconds(15)), MakeTimeAccessor(&RoutingProtocol::m_deletePeriod), MakeTimeChecker())
            .AddAttribute("NetDiameter", "Net diameter measures the maximum possible number of hops between two nodes in the network", UintegerValue(35), MakeUintegerAccessor(&RoutingProtocol::m_netDiameter), MakeUintegerChecker<uint32_t>())
            .AddAttribute("NetTraversalTime", "Estimate of the average net traversal time = 2 * NodeTraversalTime * NetDiameter", TimeValue(Seconds(2.8)), MakeTimeAccessor(&RoutingProtocol::m_netTraversalTime), MakeTimeChecker())
            .AddAttribute("PathDiscoveryTime", "Estimate of maximum time needed to find route in network = 2 * NetTraversalTime", TimeValue(Seconds(5.6)), MakeTimeAccessor(&RoutingProtocol::m_pathDiscoveryTime), MakeTimeChecker())
            .AddAttribute("MaxQueueLen", "Maximum number of packets that we allow a routing protocol to buffer.", UintegerValue(64), MakeUintegerAccessor(&RoutingProtocol::SetMaxQueueLen, &RoutingProtocol::GetMaxQueueLen), MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxQueueTime", "Maximum time packets can be queued (in seconds)", TimeValue(Seconds(30)), MakeTimeAccessor(&RoutingProtocol::SetMaxQueueTime, &RoutingProtocol::GetMaxQueueTime), MakeTimeChecker())
            .AddAttribute("AllowedHelloLoss", "Number of hello messages which may be loss for valid link.", UintegerValue(2), MakeUintegerAccessor(&RoutingProtocol::m_allowedHelloLoss), MakeUintegerChecker<uint16_t>())
            .AddAttribute("GratuitousReply", "Indicates whether a gratuitous RREP should be unicast to the node originated route discovery.", BooleanValue(true), MakeBooleanAccessor(&RoutingProtocol::SetGratuitousReplyFlag, &RoutingProtocol::GetGratuitousReplyFlag), MakeBooleanChecker())
            .AddAttribute("DestinationOnly", "Indicates only the destination may respond to this RREQ.", BooleanValue(false), MakeBooleanAccessor(&RoutingProtocol::SetDestinationOnlyFlag, &RoutingProtocol::GetDestinationOnlyFlag), MakeBooleanChecker())
            .AddAttribute("EnableHello", "Indicates whether a hello messages enable.", BooleanValue(true), MakeBooleanAccessor(&RoutingProtocol::SetHelloEnable, &RoutingProtocol::GetHelloEnable), MakeBooleanChecker())
            .AddAttribute("EnableBroadcast", "Indicates whether a broadcast data packets forwarding enable.", BooleanValue(true), MakeBooleanAccessor(&RoutingProtocol::SetBroadcastEnable, &RoutingProtocol::GetBroadcastEnable), MakeBooleanChecker())
            .AddAttribute("UniformRv", "Access to the underlying UniformRandomVariable", StringValue("ns3::UniformRandomVariable"), MakePointerAccessor(&RoutingProtocol::m_uniformRandomVariable), MakePointerChecker<UniformRandomVariable>())
            .AddAttribute("EnableFuzzy", "True to use Modified Fuzzy (Smart Delay & Suppression), False for Original Paper (Static Thresholds)", BooleanValue(true), MakeBooleanAccessor(&RoutingProtocol::m_enableFuzzy), MakeBooleanChecker());
    return tid;
}

void RoutingProtocol::SetMaxQueueLen(uint32_t len) { m_maxQueueLen = len; m_queue.SetMaxQueueLen(len); }
void RoutingProtocol::SetMaxQueueTime(Time t) { m_maxQueueTime = t; m_queue.SetQueueTimeout(t); }

RoutingProtocol::~RoutingProtocol() {}

void RoutingProtocol::DoDispose()
{
    m_ipv4 = nullptr;
    for (auto iter = m_socketAddresses.begin(); iter != m_socketAddresses.end(); iter++) iter->first->Close();
    m_socketAddresses.clear();
    for (auto iter = m_socketSubnetBroadcastAddresses.begin(); iter != m_socketSubnetBroadcastAddresses.end(); iter++) iter->first->Close();
    m_socketSubnetBroadcastAddresses.clear();
    Ipv4RoutingProtocol::DoDispose();
}

void RoutingProtocol::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    *stream->GetStream() << "Node: " << m_ipv4->GetObject<Node>()->GetId() << "; Time: " << Now().As(unit) << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit) << ", AODV Routing table" << std::endl;
    m_routingTable.Print(stream, unit);
    *stream->GetStream() << std::endl;
}

int64_t RoutingProtocol::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uniformRandomVariable->SetStream(stream);
    return 1;
}

void RoutingProtocol::Start()
{
    NS_LOG_FUNCTION(this);
    
    // EOCW Init
    Ptr<Node> node = GetObject<Node>();
    if (node) {
        Ptr<energy::EnergySourceContainer> esc = node->GetObject<energy::EnergySourceContainer>();
        if (esc && esc->GetN() > 0) {
            m_energySource = esc->Get(0);
            m_initialEnergy = m_energySource->GetInitialEnergy();
        } else {
            m_energySource = nullptr;
            m_initialEnergy = 0;
        }
    }

    if (m_enableHello) m_nb.ScheduleTimer();
    m_rreqRateLimitTimer.SetFunction(&RoutingProtocol::RreqRateLimitTimerExpire, this);
    m_rreqRateLimitTimer.Schedule(Seconds(1));
    m_rerrRateLimitTimer.SetFunction(&RoutingProtocol::RerrRateLimitTimerExpire, this);
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

// ... RouteOutput, DeferredRouteOutput, RouteInput (Standard AODV logic omitted for brevity, identical to original) ...
// TO SAVE SPACE, I AM NOT REPEATING STANDARD FUNCTIONS THAT WERE NOT MODIFIED.
// BUT SINCE YOU NEED THE FULL FILE, I MUST INCLUDE THEM. 
// Standard Routing Logic Below:

Ptr<Ipv4Route> RoutingProtocol::RouteOutput(Ptr<Packet> p, const Ipv4Header& header, Ptr<NetDevice> oif, Socket::SocketErrno& sockerr)
{
    NS_LOG_FUNCTION(this << header << (oif ? oif->GetIfIndex() : 0));
    if (!p) return LoopbackRoute(header, oif);
    if (m_socketAddresses.empty()) { sockerr = Socket::ERROR_NOROUTETOHOST; return Ptr<Ipv4Route>(); }
    sockerr = Socket::ERROR_NOTERROR;
    Ptr<Ipv4Route> route;
    Ipv4Address dst = header.GetDestination();
    RoutingTableEntry rt;
    if (m_routingTable.LookupValidRoute(dst, rt)) {
        route = rt.GetRoute();
        if (oif && route->GetOutputDevice() != oif) { sockerr = Socket::ERROR_NOROUTETOHOST; return Ptr<Ipv4Route>(); }
        UpdateRouteLifeTime(dst, m_activeRouteTimeout);
        UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
        return route;
    }
    uint32_t iif = (oif ? m_ipv4->GetInterfaceForDevice(oif) : -1);
    DeferredRouteOutputTag tag(iif);
    if (!p->PeekPacketTag(tag)) p->AddPacketTag(tag);
    return LoopbackRoute(header, oif);
}

void RoutingProtocol::DeferredRouteOutput(Ptr<const Packet> p, const Ipv4Header& header, UnicastForwardCallback ucb, ErrorCallback ecb)
{
    QueueEntry newEntry(p, header, ucb, ecb);
    if (m_queue.Enqueue(newEntry)) {
        RoutingTableEntry rt;
        if (!m_routingTable.LookupRoute(header.GetDestination(), rt) || ((rt.GetFlag() != IN_SEARCH))) {
            SendRequest(header.GetDestination());
        }
    }
}

bool RoutingProtocol::RouteInput(Ptr<const Packet> p, const Ipv4Header& header, Ptr<const NetDevice> idev, const UnicastForwardCallback& ucb, const MulticastForwardCallback& mcb, const LocalDeliverCallback& lcb, const ErrorCallback& ecb)
{
    if (m_socketAddresses.empty()) return false;
    int32_t iif = m_ipv4->GetInterfaceForDevice(idev);
    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();

    if (idev == m_lo) {
        DeferredRouteOutputTag tag;
        if (p->PeekPacketTag(tag)) { DeferredRouteOutput(p, header, ucb, ecb); return true; }
    }
    if (IsMyOwnAddress(origin)) return true;
    if (dst.IsMulticast()) return false;

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j) {
        Ipv4InterfaceAddress iface = j->second;
        if (m_ipv4->GetInterfaceForAddress(iface.GetLocal()) == iif) {
            if (dst == iface.GetBroadcast() || dst.IsBroadcast()) {
                if (m_dpd.IsDuplicate(p, header)) return true;
                UpdateRouteLifeTime(origin, m_activeRouteTimeout);
                if (!lcb.IsNull()) lcb(p, header, iif);
                if (!m_enableBroadcast) return true;
                if (header.GetTtl() > 1) {
                    RoutingTableEntry toBroadcast;
                    if (m_routingTable.LookupRoute(dst, toBroadcast)) ucb(toBroadcast.GetRoute(), p->Copy(), header);
                }
                return true;
            }
        }
    }

    if (m_ipv4->IsDestinationAddress(dst, iif)) {
        UpdateRouteLifeTime(origin, m_activeRouteTimeout);
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupValidRoute(origin, toOrigin)) {
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
        }
        if (!lcb.IsNull()) lcb(p, header, iif);
        return true;
    }

    if (m_ipv4->IsForwarding(iif)) return Forwarding(p, header, ucb, ecb);
    return false;
}

bool RoutingProtocol::Forwarding(Ptr<const Packet> p, const Ipv4Header& header, UnicastForwardCallback ucb, ErrorCallback ecb)
{
    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();
    m_routingTable.Purge();
    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst)) {
        if (toDst.GetFlag() == VALID) {
            Ptr<Ipv4Route> route = toDst.GetRoute();
            UpdateRouteLifeTime(origin, m_activeRouteTimeout);
            UpdateRouteLifeTime(dst, m_activeRouteTimeout);
            UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
            RoutingTableEntry toOrigin;
            m_routingTable.LookupRoute(origin, toOrigin);
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);
            m_nb.Update(route->GetGateway(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
            ucb(route, p, header);
            return true;
        } else {
            if (toDst.GetValidSeqNo()) {
                SendRerrWhenNoRouteToForward(dst, toDst.GetSeqNo(), origin);
                return false;
            }
        }
    }
    SendRerrWhenNoRouteToForward(dst, 0, origin);
    return false;
}

void RoutingProtocol::SetIpv4(Ptr<Ipv4> ipv4)
{
    m_ipv4 = ipv4;
    m_lo = m_ipv4->GetNetDevice(0);
    RoutingTableEntry rt(m_lo, Ipv4Address::GetLoopback(), true, 0, Ipv4InterfaceAddress(Ipv4Address::GetLoopback(), Ipv4Mask("255.0.0.0")), 1, Ipv4Address::GetLoopback(), Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);
    Simulator::ScheduleNow(&RoutingProtocol::Start, this);
}

void RoutingProtocol::NotifyInterfaceUp(uint32_t i)
{
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
    if (iface.GetLocal() == Ipv4Address("127.0.0.1")) return;

    Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAodv, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetLocal(), AODV_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketAddresses.insert(std::make_pair(socket, iface));

    socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAodv, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetBroadcast(), AODV_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
    RoutingTableEntry rt(dev, iface.GetBroadcast(), true, 0, iface, 1, iface.GetBroadcast(), Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    if (l3->GetInterface(i)->GetArpCache()) m_nb.AddArpCache(l3->GetInterface(i)->GetArpCache());
    
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (wifi) {
        Ptr<WifiMac> mac = wifi->GetMac();
        if (mac) mac->TraceConnectWithoutContext("DroppedMpdu", MakeCallback(&RoutingProtocol::NotifyTxError, this));
    }
}

void RoutingProtocol::NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu) { m_nb.GetTxErrorCallback()(mpdu->GetHeader()); }

void RoutingProtocol::NotifyInterfaceDown(uint32_t i)
{
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    Ptr<NetDevice> dev = l3->GetNetDevice(i);
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (wifi) {
        Ptr<WifiMac> mac = wifi->GetMac()->GetObject<AdhocWifiMac>();
        if (mac) {
            mac->TraceDisconnectWithoutContext("DroppedMpdu", MakeCallback(&RoutingProtocol::NotifyTxError, this));
            m_nb.DelArpCache(l3->GetInterface(i)->GetArpCache());
        }
    }
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    if (socket) { socket->Close(); m_socketAddresses.erase(socket); }
    socket = FindSubnetBroadcastSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    if (socket) { socket->Close(); m_socketSubnetBroadcastAddresses.erase(socket); }
    if (m_socketAddresses.empty()) { m_htimer.Cancel(); m_nb.Clear(); m_routingTable.Clear(); return; }
    m_routingTable.DeleteAllRoutesFromInterface(m_ipv4->GetAddress(i, 0));
}

void RoutingProtocol::NotifyAddAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (!l3->IsUp(i)) return;
    if (l3->GetNAddresses(i) == 1) {
        Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
        if (iface.GetLocal() == Ipv4Address("127.0.0.1")) return;
        Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
        socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAodv, this));
        socket->BindToNetDevice(l3->GetNetDevice(i));
        socket->Bind(InetSocketAddress(iface.GetLocal(), AODV_PORT));
        socket->SetAllowBroadcast(true);
        m_socketAddresses.insert(std::make_pair(socket, iface));

        socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
        socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAodv, this));
        socket->BindToNetDevice(l3->GetNetDevice(i));
        socket->Bind(InetSocketAddress(iface.GetBroadcast(), AODV_PORT));
        socket->SetAllowBroadcast(true);
        socket->SetIpRecvTtl(true);
        m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
        RoutingTableEntry rt(dev, iface.GetBroadcast(), true, 0, iface, 1, iface.GetBroadcast(), Simulator::GetMaximumSimulationTime());
        m_routingTable.AddRoute(rt);
    }
}

void RoutingProtocol::NotifyRemoveAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(address);
    if (socket) {
        m_routingTable.DeleteAllRoutesFromInterface(address);
        socket->Close(); m_socketAddresses.erase(socket);
        Ptr<Socket> unicastSocket = FindSubnetBroadcastSocketWithInterfaceAddress(address);
        if (unicastSocket) { unicastSocket->Close(); m_socketAddresses.erase(unicastSocket); }
        if (m_socketAddresses.empty()) { m_htimer.Cancel(); m_nb.Clear(); m_routingTable.Clear(); return; }
    }
}

bool RoutingProtocol::IsMyOwnAddress(Ipv4Address src)
{
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        if (src == j->second.GetLocal()) return true;
    return false;
}

Ptr<Ipv4Route> RoutingProtocol::LoopbackRoute(const Ipv4Header& hdr, Ptr<NetDevice> oif) const
{
    Ptr<Ipv4Route> rt = Create<Ipv4Route>();
    rt->SetDestination(hdr.GetDestination());
    auto j = m_socketAddresses.begin();
    if (oif) {
        for (j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j) {
            Ipv4Address addr = j->second.GetLocal();
            int32_t interface = m_ipv4->GetInterfaceForAddress(addr);
            if (oif == m_ipv4->GetNetDevice(static_cast<uint32_t>(interface))) { rt->SetSource(addr); break; }
        }
    } else { rt->SetSource(j->second.GetLocal()); }
    rt->SetGateway(Ipv4Address("127.0.0.1"));
    rt->SetOutputDevice(m_lo);
    return rt;
}

void RoutingProtocol::SendRequest(Ipv4Address dst)
{
    if (m_rreqCount == m_rreqRateLimit) {
        Simulator::Schedule(m_rreqRateLimitTimer.GetDelayLeft() + MicroSeconds(100), &RoutingProtocol::SendRequest, this, dst);
        return;
    } else { m_rreqCount++; }

    RreqHeader rreqHeader;
    rreqHeader.SetDst(dst);
    RoutingTableEntry rt;
    uint16_t ttl = m_ttlStart;
    if (m_routingTable.LookupRoute(dst, rt)) {
        if (rt.GetFlag() != IN_SEARCH) ttl = std::min<uint16_t>(rt.GetHop() + m_ttlIncrement, m_netDiameter);
        else {
            ttl = rt.GetHop() + m_ttlIncrement;
            if (ttl > m_ttlThreshold) ttl = m_netDiameter;
        }
        if (ttl == m_netDiameter) rt.IncrementRreqCnt();
        if (rt.GetValidSeqNo()) rreqHeader.SetDstSeqno(rt.GetSeqNo());
        else rreqHeader.SetUnknownSeqno(true);
        rt.SetHop(ttl); rt.SetFlag(IN_SEARCH); rt.SetLifeTime(m_pathDiscoveryTime);
        m_routingTable.Update(rt);
    } else {
        rreqHeader.SetUnknownSeqno(true);
        RoutingTableEntry newEntry(nullptr, dst, false, 0, Ipv4InterfaceAddress(), ttl, Ipv4Address(), m_pathDiscoveryTime);
        if (ttl == m_netDiameter) newEntry.IncrementRreqCnt();
        newEntry.SetFlag(IN_SEARCH);
        m_routingTable.AddRoute(newEntry);
    }

    if (m_gratuitousReply) rreqHeader.SetGratuitousRrep(true);
    if (m_destinationOnly) rreqHeader.SetDestinationOnly(true);
    m_seqNo++; rreqHeader.SetOriginSeqno(m_seqNo);
    m_requestId++; rreqHeader.SetId(m_requestId);

    // EOCW Init
    rreqHeader.m_pathMinEnergy = GetResidualEnergyScore();
    rreqHeader.m_pathAvgCongestion = GetCongestionDegreeScore();

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j) {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        rreqHeader.SetOrigin(iface.GetLocal());
        m_rreqIdCache.IsDuplicate(iface.GetLocal(), m_requestId);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag; tag.SetTtl(ttl); packet->AddPacketTag(tag);
        packet->AddHeader(rreqHeader);
        packet->AddHeader(TypeHeader(AODVTYPE_RREQ));
        Ipv4Address destination = (iface.GetMask() == Ipv4Mask::GetOnes()) ? Ipv4Address("255.255.255.255") : iface.GetBroadcast();
        m_lastBcastTime = Simulator::Now();
        Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))), &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
    ScheduleRreqRetry(dst);
}

void RoutingProtocol::SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination)
{
    // Safety check: Don't send if interface is down
    Ptr<Node> node = GetObject<Node>();
    if (node) {
        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        if (ipv4 && ipv4->GetNInterfaces() > 1) {
            if (!ipv4->IsUp(1)) return; // WiFi Interface Down
        }
    }
    socket->SendTo(packet, 0, InetSocketAddress(destination, AODV_PORT));
}

void RoutingProtocol::ScheduleRreqRetry(Ipv4Address dst)
{
    if (m_addressReqTimer.find(dst) == m_addressReqTimer.end()) {
        Timer timer(Timer::CANCEL_ON_DESTROY);
        m_addressReqTimer[dst] = timer;
    }
    m_addressReqTimer[dst].SetFunction(&RoutingProtocol::RouteRequestTimerExpire, this);
    m_addressReqTimer[dst].Cancel();
    m_addressReqTimer[dst].SetArguments(dst);
    RoutingTableEntry rt;
    m_routingTable.LookupRoute(dst, rt);
    Time retry;
    if (rt.GetHop() < m_netDiameter) retry = 2 * m_nodeTraversalTime * (rt.GetHop() + m_timeoutBuffer);
    else {
        uint16_t backoffFactor = rt.GetRreqCnt() - 1;
        retry = m_netTraversalTime * (1 << backoffFactor);
    }
    m_addressReqTimer[dst].Schedule(retry);
}

void RoutingProtocol::RecvAodv(Ptr<Socket> socket)
{
    Address sourceAddress;
    Ptr<Packet> packet = socket->RecvFrom(sourceAddress);
    InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom(sourceAddress);
    Ipv4Address sender = inetSourceAddr.GetIpv4();
    Ipv4Address receiver;
    if (m_socketAddresses.find(socket) != m_socketAddresses.end()) receiver = m_socketAddresses[socket].GetLocal();
    else if (m_socketSubnetBroadcastAddresses.find(socket) != m_socketSubnetBroadcastAddresses.end()) receiver = m_socketSubnetBroadcastAddresses[socket].GetLocal();
    else return;

    UpdateRouteToNeighbor(sender, receiver);
    TypeHeader tHeader(AODVTYPE_RREQ);
    packet->RemoveHeader(tHeader);
    if (!tHeader.IsValid()) return;
    switch (tHeader.Get()) {
        case AODVTYPE_RREQ: RecvRequest(packet, receiver, sender); break;
        case AODVTYPE_RREP: RecvReply(packet, receiver, sender); break;
        case AODVTYPE_RERR: RecvError(packet, sender); break;
        case AODVTYPE_RREP_ACK: RecvReplyAck(sender); break;
    }
}

bool RoutingProtocol::UpdateRouteLifeTime(Ipv4Address addr, Time lifetime)
{
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(addr, rt)) {
        if (rt.GetFlag() == VALID) {
            rt.SetRreqCnt(0);
            rt.SetLifeTime(std::max(lifetime, rt.GetLifeTime()));
            m_routingTable.Update(rt);
            return true;
        }
    }
    return false;
}

void RoutingProtocol::UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver)
{
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(sender, toNeighbor)) {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, sender, false, 0, m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0), 1, sender, m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    } else {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        if (toNeighbor.GetValidSeqNo() && (toNeighbor.GetHop() == 1) && (toNeighbor.GetOutputDevice() == dev)) {
            toNeighbor.SetLifeTime(std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
        } else {
            RoutingTableEntry newEntry(dev, sender, false, 0, m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0), 1, sender, std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
            m_routingTable.Update(newEntry);
        }
    }
}

void RoutingProtocol::RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
    RreqHeader rreqHeader;
    p->RemoveHeader(rreqHeader);

    RoutingTableEntry toPrev;
    if (m_routingTable.LookupRoute(src, toPrev) && toPrev.IsUnidirectional()) return;

    // === EOCW MODIFICATION: RREQ SUPPRESSION ===
    if (m_enableFuzzy) {
        bool amIDestination = IsMyOwnAddress(rreqHeader.GetDst());
        double myCurrentEnergy = GetResidualEnergyScore();
        if (!amIDestination && myCurrentEnergy < 0.20) {
            // EOCW Protection: Drop RREQ if energy < 20% and not destination
            return;
        }
    }
    // ===========================================

    uint32_t id = rreqHeader.GetId();
    Ipv4Address origin = rreqHeader.GetOrigin();

    double myEnergy = GetResidualEnergyScore();
    double myCongestion = GetCongestionDegreeScore();
    double old_pathMinEnergy = rreqHeader.m_pathMinEnergy;
    double old_pathAvgCongestion = rreqHeader.m_pathAvgCongestion;
    uint8_t old_hop_count = rreqHeader.GetHopCount();
    
    double new_pathMinEnergy = std::min(old_pathMinEnergy, myEnergy);
    uint32_t hop = old_hop_count + 1; 
    double new_pathAvgCongestion = ((old_pathAvgCongestion * old_hop_count) + myCongestion) / (double)hop;

    bool amIDestination = IsMyOwnAddress(rreqHeader.GetDst());

    if (m_rreqIdCache.IsDuplicate(origin, id)) {
        if (!amIDestination) return; 
    }

    // Update Reverse Route to Origin
    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(origin, toOrigin)) {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, origin, true, rreqHeader.GetOriginSeqno(), m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0), hop, src, Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime));
        m_routingTable.AddRoute(newEntry);
        toOrigin = newEntry;
    } else {
        if (toOrigin.GetValidSeqNo()) {
            if (int32_t(rreqHeader.GetOriginSeqno()) - int32_t(toOrigin.GetSeqNo()) > 0) toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno());
        } else { toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno()); }
        toOrigin.SetValidSeqNo(true);
        toOrigin.SetNextHop(src);
        toOrigin.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toOrigin.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toOrigin.SetHop(hop);
        toOrigin.SetLifeTime(std::max(Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime), toOrigin.GetLifeTime()));
        m_routingTable.Update(toOrigin);
    }

    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(src, toNeighbor)) {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, src, false, rreqHeader.GetOriginSeqno(), m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0), 1, src, m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    } else {
        toNeighbor.SetLifeTime(m_activeRouteTimeout);
        toNeighbor.SetValidSeqNo(false);
        toNeighbor.SetSeqNo(rreqHeader.GetOriginSeqno());
        toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1);
        toNeighbor.SetNextHop(src);
        m_routingTable.Update(toNeighbor);
    }
    m_nb.Update(src, Time(m_allowedHelloLoss * m_helloInterval));

    if (amIDestination) {
        EocwPath newPath(new_pathMinEnergy, new_pathAvgCongestion, (uint32_t)hop, toOrigin);
        m_eocwPathCache[id].push_back(newPath);
        if (m_eocwPathTimers.find(id) == m_eocwPathTimers.end()) {
            m_eocwPathTimers[id] = Timer(Timer::CANCEL_ON_DESTROY);
            m_eocwPathTimers[id].SetFunction(&RoutingProtocol::SelectBestEocwPath, this);
            m_eocwPathTimers[id].SetArguments(id, origin, rreqHeader.GetDst());
            m_eocwPathTimers[id].SetDelay(MilliSeconds(20));
            m_eocwPathTimers[id].Schedule();
        }
        return;
    }

    RoutingTableEntry toDst;
    Ipv4Address dst = rreqHeader.GetDst();
    if (m_routingTable.LookupRoute(dst, toDst)) {
        if (toDst.GetNextHop() == src) return;
        if ((rreqHeader.GetUnknownSeqno() || (int32_t(toDst.GetSeqNo()) - int32_t(rreqHeader.GetDstSeqno()) >= 0)) && toDst.GetValidSeqNo()) {
            if (!rreqHeader.GetDestinationOnly() && toDst.GetFlag() == VALID) {
                m_routingTable.LookupRoute(origin, toOrigin);
                SendReplyByIntermediateNode(toDst, toOrigin, rreqHeader.GetGratuitousRrep());
                return;
            }
            rreqHeader.SetDstSeqno(toDst.GetSeqNo());
            rreqHeader.SetUnknownSeqno(false);
        }
    }

    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2) return;

    rreqHeader.SetHopCount(hop);
    rreqHeader.m_pathMinEnergy = new_pathMinEnergy;
    rreqHeader.m_pathAvgCongestion = new_pathAvgCongestion;

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j) {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag ttl;
        ttl.SetTtl(tag.GetTtl() - 1);
        packet->AddPacketTag(ttl);
        packet->AddHeader(rreqHeader);
        packet->AddHeader(TypeHeader(AODVTYPE_RREQ));
        
        Ipv4Address destination = (iface.GetMask() == Ipv4Mask::GetOnes()) ? Ipv4Address("255.255.255.255") : iface.GetBroadcast();
        m_lastBcastTime = Simulator::Now();
        
        // === EOCW MODIFICATION: SMART DELAY ===
        Time forwardDelay;
        if (m_enableFuzzy) {
             // Modified Fuzzy: Smart Delay based on Health
             double healthPenalty = (1.0 - myEnergy) + (1.0 - myCongestion);
             forwardDelay = MilliSeconds(healthPenalty * 50) + MilliSeconds(m_uniformRandomVariable->GetInteger(0, 5));
        } else {
             // Original Paper / Standard: Random Jitter Only
             forwardDelay = MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10));
        }
        // ======================================

        Simulator::Schedule(forwardDelay, &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
}

void RoutingProtocol::SendReply(const RreqHeader& rreqHeader, const RoutingTableEntry& toOrigin)
{
    if (!rreqHeader.GetUnknownSeqno() && (rreqHeader.GetDstSeqno() == m_seqNo + 1)) m_seqNo++;
    RrepHeader rrepHeader(0, 0, rreqHeader.GetDst(), m_seqNo, toOrigin.GetDestination(), m_myRouteTimeout);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag; tag.SetTtl(toOrigin.GetHop()); packet->AddPacketTag(tag);
    packet->AddHeader(rrepHeader);
    packet->AddHeader(TypeHeader(AODVTYPE_RREP));
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), AODV_PORT));
}

void RoutingProtocol::SendReplyByIntermediateNode(RoutingTableEntry& toDst, RoutingTableEntry& toOrigin, bool gratRep)
{
    RrepHeader rrepHeader(0, toDst.GetHop(), toDst.GetDestination(), toDst.GetSeqNo(), toOrigin.GetDestination(), toDst.GetLifeTime());
    if (toDst.GetHop() == 1) {
        rrepHeader.SetAckRequired(true);
        RoutingTableEntry toNextHop;
        m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHop);
        toNextHop.m_ackTimer.SetFunction(&RoutingProtocol::AckTimerExpire, this);
        toNextHop.m_ackTimer.SetArguments(toNextHop.GetDestination(), m_blackListTimeout);
        toNextHop.m_ackTimer.SetDelay(m_nextHopWait);
    }
    toDst.InsertPrecursor(toOrigin.GetNextHop());
    toOrigin.InsertPrecursor(toDst.GetNextHop());
    m_routingTable.Update(toDst);
    m_routingTable.Update(toOrigin);

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag; tag.SetTtl(toOrigin.GetHop()); packet->AddPacketTag(tag);
    packet->AddHeader(rrepHeader);
    packet->AddHeader(TypeHeader(AODVTYPE_RREP));
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), AODV_PORT));

    if (gratRep) {
        RrepHeader gratRepHeader(0, toOrigin.GetHop(), toOrigin.GetDestination(), toOrigin.GetSeqNo(), toDst.GetDestination(), toOrigin.GetLifeTime());
        Ptr<Packet> packetToDst = Create<Packet>();
        SocketIpTtlTag gratTag; gratTag.SetTtl(toDst.GetHop()); packetToDst->AddPacketTag(gratTag);
        packetToDst->AddHeader(gratRepHeader);
        packetToDst->AddHeader(TypeHeader(AODVTYPE_RREP));
        socket = FindSocketWithInterfaceAddress(toDst.GetInterface());
        socket->SendTo(packetToDst, 0, InetSocketAddress(toDst.GetNextHop(), AODV_PORT));
    }
}

void RoutingProtocol::SendReplyAck(Ipv4Address neighbor)
{
    RrepAckHeader h;
    TypeHeader typeHeader(AODVTYPE_RREP_ACK);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag; tag.SetTtl(1); packet->AddPacketTag(tag);
    packet->AddHeader(h); packet->AddHeader(typeHeader);
    RoutingTableEntry toNeighbor;
    m_routingTable.LookupRoute(neighbor, toNeighbor);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toNeighbor.GetInterface());
    socket->SendTo(packet, 0, InetSocketAddress(neighbor, AODV_PORT));
}

void RoutingProtocol::RecvReply(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender)
{
    RrepHeader rrepHeader;
    p->RemoveHeader(rrepHeader);
    Ipv4Address dst = rrepHeader.GetDst();
    uint8_t hop = rrepHeader.GetHopCount() + 1;
    rrepHeader.SetHopCount(hop);

    if (dst == rrepHeader.GetOrigin()) { ProcessHello(rrepHeader, receiver); return; }

    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
    RoutingTableEntry newEntry(dev, dst, true, rrepHeader.GetDstSeqno(), m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0), hop, sender, rrepHeader.GetLifeTime());
    
    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst)) {
        if ((!toDst.GetValidSeqNo()) || ((int32_t(rrepHeader.GetDstSeqno()) - int32_t(toDst.GetSeqNo())) > 0) || (rrepHeader.GetDstSeqno() == toDst.GetSeqNo() && toDst.GetFlag() != VALID) || (rrepHeader.GetDstSeqno() == toDst.GetSeqNo() && hop < toDst.GetHop())) {
            newEntry.m_pathMinEnergy = rrepHeader.m_pathMinEnergy;
            newEntry.m_pathAvgCongestion = rrepHeader.m_pathAvgCongestion;
            m_routingTable.Update(newEntry);
        }
    } else {
        newEntry.m_pathMinEnergy = rrepHeader.m_pathMinEnergy;
        newEntry.m_pathAvgCongestion = rrepHeader.m_pathAvgCongestion;
        m_routingTable.AddRoute(newEntry);
    }
    if (rrepHeader.GetAckRequired()) { SendReplyAck(sender); rrepHeader.SetAckRequired(false); }

    if (IsMyOwnAddress(rrepHeader.GetOrigin())) {
        if (toDst.GetFlag() == IN_SEARCH) {
            newEntry.m_pathMinEnergy = rrepHeader.m_pathMinEnergy;
            newEntry.m_pathAvgCongestion = rrepHeader.m_pathAvgCongestion;
            m_routingTable.Update(newEntry);
            m_addressReqTimer[dst].Cancel(); m_addressReqTimer.erase(dst);
        }
        m_routingTable.LookupRoute(dst, toDst);
        SendPacketFromQueue(dst, toDst.GetRoute());
        return;
    }

    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(rrepHeader.GetOrigin(), toOrigin) || toOrigin.GetFlag() == IN_SEARCH) return;
    toOrigin.SetLifeTime(std::max(m_activeRouteTimeout, toOrigin.GetLifeTime()));
    m_routingTable.Update(toOrigin);

    if (m_routingTable.LookupValidRoute(rrepHeader.GetDst(), toDst)) {
        toDst.InsertPrecursor(toOrigin.GetNextHop()); m_routingTable.Update(toDst);
        RoutingTableEntry toNextHopToDst; m_routingTable.LookupRoute(toDst.GetNextHop(), toNextHopToDst);
        toNextHopToDst.InsertPrecursor(toOrigin.GetNextHop()); m_routingTable.Update(toNextHopToDst);
        toOrigin.InsertPrecursor(toDst.GetNextHop()); m_routingTable.Update(toOrigin);
        RoutingTableEntry toNextHopToOrigin; m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHopToOrigin);
        toNextHopToOrigin.InsertPrecursor(toDst.GetNextHop()); m_routingTable.Update(toNextHopToOrigin);
    }
    
    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2) return;

    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag ttl; ttl.SetTtl(tag.GetTtl() - 1); packet->AddPacketTag(ttl);
    packet->AddHeader(rrepHeader); packet->AddHeader(TypeHeader(AODVTYPE_RREP));
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), AODV_PORT));
}

void RoutingProtocol::RecvReplyAck(Ipv4Address neighbor)
{
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(neighbor, rt)) { rt.m_ackTimer.Cancel(); rt.SetFlag(VALID); m_routingTable.Update(rt); }
}

void RoutingProtocol::ProcessHello(const RrepHeader& rrepHeader, Ipv4Address receiver)
{
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(rrepHeader.GetDst(), toNeighbor)) {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, rrepHeader.GetDst(), true, rrepHeader.GetDstSeqno(), m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0), 1, rrepHeader.GetDst(), rrepHeader.GetLifeTime());
        m_routingTable.AddRoute(newEntry);
    } else {
        toNeighbor.SetLifeTime(std::max(Time(m_allowedHelloLoss * m_helloInterval), toNeighbor.GetLifeTime()));
        toNeighbor.SetSeqNo(rrepHeader.GetDstSeqno()); toNeighbor.SetValidSeqNo(true); toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1); toNeighbor.SetNextHop(rrepHeader.GetDst());
        m_routingTable.Update(toNeighbor);
    }
    if (m_enableHello) m_nb.Update(rrepHeader.GetDst(), Time(m_allowedHelloLoss * m_helloInterval));
}

void RoutingProtocol::RecvError(Ptr<Packet> p, Ipv4Address src)
{
    RerrHeader rerrHeader;
    p->RemoveHeader(rerrHeader);
    std::map<Ipv4Address, uint32_t> dstWithNextHopSrc;
    std::map<Ipv4Address, uint32_t> unreachable;
    m_routingTable.GetListOfDestinationWithNextHop(src, dstWithNextHopSrc);
    std::pair<Ipv4Address, uint32_t> un;
    while (rerrHeader.RemoveUnDestination(un)) {
        for (auto i = dstWithNextHopSrc.begin(); i != dstWithNextHopSrc.end(); ++i) {
            if (i->first == un.first) unreachable.insert(un);
        }
    }
    std::vector<Ipv4Address> precursors;
    for (auto i = unreachable.begin(); i != unreachable.end();) {
        if (!rerrHeader.AddUnDestination(i->first, i->second)) {
            TypeHeader typeHeader(AODVTYPE_RERR); Ptr<Packet> packet = Create<Packet>(); SocketIpTtlTag tag; tag.SetTtl(1);
            packet->AddPacketTag(tag); packet->AddHeader(rerrHeader); packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors); rerrHeader.Clear();
        } else {
            RoutingTableEntry toDst; m_routingTable.LookupRoute(i->first, toDst); toDst.GetPrecursors(precursors); ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0) {
        TypeHeader typeHeader(AODVTYPE_RERR); Ptr<Packet> packet = Create<Packet>(); SocketIpTtlTag tag; tag.SetTtl(1);
        packet->AddPacketTag(tag); packet->AddHeader(rerrHeader); packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

void RoutingProtocol::RouteRequestTimerExpire(Ipv4Address dst)
{
    RoutingTableEntry toDst;
    if (m_routingTable.LookupValidRoute(dst, toDst)) { SendPacketFromQueue(dst, toDst.GetRoute()); return; }
    if (toDst.GetRreqCnt() == m_rreqRetries) {
        m_addressReqTimer.erase(dst); m_routingTable.DeleteRoute(dst); m_queue.DropPacketWithDst(dst); return;
    }
    if (toDst.GetFlag() == IN_SEARCH) SendRequest(dst);
    else { m_addressReqTimer.erase(dst); m_routingTable.DeleteRoute(dst); m_queue.DropPacketWithDst(dst); }
}

void RoutingProtocol::HelloTimerExpire()
{
    Time offset = Time(Seconds(0));
    if (m_lastBcastTime > Time(Seconds(0))) offset = Simulator::Now() - m_lastBcastTime;
    else SendHello();
    m_htimer.Cancel();
    Time diff = m_helloInterval - offset;
    m_htimer.Schedule(std::max(Time(Seconds(0)), diff));
    m_lastBcastTime = Time(Seconds(0));
}

void RoutingProtocol::RreqRateLimitTimerExpire() { m_rreqCount = 0; m_rreqRateLimitTimer.Schedule(Seconds(1)); }
void RoutingProtocol::RerrRateLimitTimerExpire() { m_rerrCount = 0; m_rerrRateLimitTimer.Schedule(Seconds(1)); }
void RoutingProtocol::AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout) { m_routingTable.MarkLinkAsUnidirectional(neighbor, blacklistTimeout); }

void RoutingProtocol::SendHello()
{
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j) {
        Ptr<Socket> socket = j->first; Ipv4InterfaceAddress iface = j->second;
        RrepHeader helloHeader(0, 0, iface.GetLocal(), m_seqNo, iface.GetLocal(), Time(m_allowedHelloLoss * m_helloInterval));
        Ptr<Packet> packet = Create<Packet>(); SocketIpTtlTag tag; tag.SetTtl(1); packet->AddPacketTag(tag);
        packet->AddHeader(helloHeader); packet->AddHeader(TypeHeader(AODVTYPE_RREP));
        Ipv4Address destination = (iface.GetMask() == Ipv4Mask::GetOnes()) ? Ipv4Address("255.255.255.255") : iface.GetBroadcast();
        Time jitter = Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)));
        Simulator::Schedule(jitter, &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
}

void RoutingProtocol::SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route)
{
    QueueEntry queueEntry;
    while (m_queue.Dequeue(dst, queueEntry)) {
        DeferredRouteOutputTag tag;
        Ptr<Packet> p = ConstCast<Packet>(queueEntry.GetPacket());
        if (p->RemovePacketTag(tag) && tag.GetInterface() != -1 && tag.GetInterface() != m_ipv4->GetInterfaceForDevice(route->GetOutputDevice())) return;
        UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback();
        Ipv4Header header = queueEntry.GetIpv4Header();
        header.SetSource(route->GetSource());
        header.SetTtl(header.GetTtl() + 1);
        ucb(route, p, header);
    }
}

void RoutingProtocol::SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop)
{
    RerrHeader rerrHeader;
    std::vector<Ipv4Address> precursors;
    std::map<Ipv4Address, uint32_t> unreachable;
    RoutingTableEntry toNextHop;
    if (!m_routingTable.LookupRoute(nextHop, toNextHop)) return;
    toNextHop.GetPrecursors(precursors);
    rerrHeader.AddUnDestination(nextHop, toNextHop.GetSeqNo());
    m_routingTable.GetListOfDestinationWithNextHop(nextHop, unreachable);
    for (auto i = unreachable.begin(); i != unreachable.end();) {
        if (!rerrHeader.AddUnDestination(i->first, i->second)) {
            TypeHeader typeHeader(AODVTYPE_RERR); Ptr<Packet> packet = Create<Packet>(); SocketIpTtlTag tag; tag.SetTtl(1);
            packet->AddPacketTag(tag); packet->AddHeader(rerrHeader); packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors); rerrHeader.Clear();
        } else {
            RoutingTableEntry toDst; m_routingTable.LookupRoute(i->first, toDst); toDst.GetPrecursors(precursors); ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0) {
        TypeHeader typeHeader(AODVTYPE_RERR); Ptr<Packet> packet = Create<Packet>(); SocketIpTtlTag tag; tag.SetTtl(1);
        packet->AddPacketTag(tag); packet->AddHeader(rerrHeader); packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    unreachable.insert(std::make_pair(nextHop, toNextHop.GetSeqNo()));
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

void RoutingProtocol::SendRerrWhenNoRouteToForward(Ipv4Address dst, uint32_t dstSeqNo, Ipv4Address origin)
{
    if (m_rerrCount == m_rerrRateLimit) return;
    RerrHeader rerrHeader; rerrHeader.AddUnDestination(dst, dstSeqNo);
    RoutingTableEntry toOrigin; Ptr<Packet> packet = Create<Packet>(); SocketIpTtlTag tag; tag.SetTtl(1);
    packet->AddPacketTag(tag); packet->AddHeader(rerrHeader); packet->AddHeader(TypeHeader(AODVTYPE_RERR));
    if (m_routingTable.LookupValidRoute(origin, toOrigin)) {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
        socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), AODV_PORT));
    } else {
        for (auto i = m_socketAddresses.begin(); i != m_socketAddresses.end(); ++i) {
            Ptr<Socket> socket = i->first; Ipv4InterfaceAddress iface = i->second;
            Ipv4Address destination = (iface.GetMask() == Ipv4Mask::GetOnes()) ? Ipv4Address("255.255.255.255") : iface.GetBroadcast();
            socket->SendTo(packet->Copy(), 0, InetSocketAddress(destination, AODV_PORT));
        }
    }
}

void RoutingProtocol::SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors)
{
    if (precursors.empty() || m_rerrCount == m_rerrRateLimit) return;
    if (precursors.size() == 1) {
        RoutingTableEntry toPrecursor;
        if (m_routingTable.LookupValidRoute(precursors.front(), toPrecursor)) {
            Ptr<Socket> socket = FindSocketWithInterfaceAddress(toPrecursor.GetInterface());
            Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))), &RoutingProtocol::SendTo, this, socket, packet, precursors.front());
            m_rerrCount++;
        }
        return;
    }
    std::vector<Ipv4InterfaceAddress> ifaces;
    RoutingTableEntry toPrecursor;
    for (auto i = precursors.begin(); i != precursors.end(); ++i) {
        if (m_routingTable.LookupValidRoute(*i, toPrecursor) && std::find(ifaces.begin(), ifaces.end(), toPrecursor.GetInterface()) == ifaces.end()) ifaces.push_back(toPrecursor.GetInterface());
    }
    for (auto i = ifaces.begin(); i != ifaces.end(); ++i) {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(*i);
        Ipv4Address destination = (i->GetMask() == Ipv4Mask::GetOnes()) ? Ipv4Address("255.255.255.255") : i->GetBroadcast();
        Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))), &RoutingProtocol::SendTo, this, socket, packet->Copy(), destination);
    }
}

Ptr<Socket> RoutingProtocol::FindSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j) if (j->second == addr) return j->first;
    return nullptr;
}

Ptr<Socket> RoutingProtocol::FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    for (auto j = m_socketSubnetBroadcastAddresses.begin(); j != m_socketSubnetBroadcastAddresses.end(); ++j) if (j->second == addr) return j->first;
    return nullptr;
}

void RoutingProtocol::DoInitialize()
{
    if (m_enableHello) {
        m_htimer.SetFunction(&RoutingProtocol::HelloTimerExpire, this);
        m_htimer.Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 100)));
    }
    Ipv4RoutingProtocol::DoInitialize();
}

// ============================================================================
// EOCW / FUZZY IMPLEMENTATION FUNCTIONS
// ============================================================================

double RoutingProtocol::FuzzyTriangle(double value, double a, double b, double c)
{
    if (value <= a || value >= c) return 0.0;
    if (value == b) return 1.0;
    if (value < b) return (value - a) / (b - a);
    return (c - value) / (c - b);
}

double RoutingProtocol::GetResidualEnergyScore()
{
    if (!m_energySource || m_initialEnergy == 0) return 1.0;
    return m_energySource->GetRemainingEnergy() / m_initialEnergy;
}

double RoutingProtocol::GetCongestionDegreeScore()
{
    if (m_socketAddresses.empty()) return 1.0;
    for (auto const& [socket, iface] : m_socketAddresses) {
        if (!m_ipv4) continue;
        int32_t i = m_ipv4->GetInterfaceForAddress(iface.GetLocal());
        if (i < 0) continue;
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(static_cast<uint32_t>(i));
        if (!dev) continue;
        Ptr<WifiNetDevice> wifiDev = dev->GetObject<WifiNetDevice>();
        if (wifiDev) {
            Ptr<WifiMac> mac = wifiDev->GetMac();
            if (!mac) continue;
            Ptr<AdhocWifiMac> adhocMac = mac->GetObject<AdhocWifiMac>();
            if (adhocMac) {
                Ptr<WifiMacQueue> queue = adhocMac->GetTxopQueue(ns3::AC_BE);
                if (queue) {
                    double l_all = (double)queue->GetMaxSize().GetValue();
                    if (l_all == 0) return 1.0;
                    double l_current = (double)queue->GetCurrentSize().GetValue();
                    return std::max(0.0, (l_all - l_current) / l_all);
                }
            }
        }
    }
    return 1.0;
}

double RoutingProtocol::GetHopCountScore(uint32_t hopCount)
{
    if (hopCount <= 2) return 1.0;
    if (hopCount <= 4) return 0.6;
    if (hopCount <= 6) return 0.4;
    return 0.1;
}

std::vector<double> RoutingProtocol::GetEwmWeights(const std::vector<EocwPath>& paths)
{
    int m = paths.size(); int n = 3;
    if (m <= 1) return {0.333, 0.333, 0.333};
    std::vector<std::vector<double>> X(m, std::vector<double>(n));
    for (int i = 0; i < m; ++i) {
        X[i][0] = paths[i].pathAvgCongestion; X[i][1] = paths[i].pathMinEnergy; X[i][2] = GetHopCountScore(paths[i].hopCount);
    }
    std::vector<double> H(n, 0.0);
    double k = 1.0 / std::log(m);
    for (int j = 0; j < n; ++j) {
        double sumYij = 0.0; for (int i = 0; i < m; ++i) sumYij += X[i][j];
        if (sumYij == 0) continue;
        double sumPijLnPij = 0.0;
        for (int i = 0; i < m; ++i) {
            double pij = X[i][j] / sumYij;
            if (pij > 0) sumPijLnPij += pij * std::log(pij);
        }
        H[j] = -k * sumPijLnPij;
    }
    std::vector<double> d(n); double sumD = 0.0;
    for (int j = 0; j < n; ++j) { d[j] = 1.0 - H[j]; sumD += d[j]; }
    std::vector<double> mu(n);
    if (sumD == 0) { std::fill(mu.begin(), mu.end(), 1.0/n); return mu; }
    for (int j = 0; j < n; ++j) mu[j] = d[j] / sumD;
    return mu;
}

double RoutingProtocol::CalculateEocwScore(const EocwPath& path, const std::vector<double>& ahp_w, const std::vector<double>& ewm_mu)
{
    double s_cd = path.pathAvgCongestion; double s_re = path.pathMinEnergy; double s_rh = GetHopCountScore(path.hopCount);
    double w_cd = ahp_w[0] * ewm_mu[0]; double w_re = ahp_w[1] * ewm_mu[1]; double w_rh = ahp_w[2] * ewm_mu[2];
    double sumW = w_cd + w_re + w_rh;
    if (sumW == 0) return 0;
    return ((w_cd/sumW) * s_cd) + ((w_re/sumW) * s_re) + ((w_rh/sumW) * s_rh);
}

std::vector<double> RoutingProtocol::GetFuzzyWeights(double re, double cd_score)
{
    if (!m_enableFuzzy) {
        // === ORIGINAL PAPER / STATIC AHP LOGIC ===
        if (re >= 0.8) return {0.5396, 0.297, 0.1634};
        else if (re >= 0.5) return {0.637, 0.2583, 0.1047};
        // Flawed Paper Logic: Favors congestion over energy when low
        else if (re <= 0.3) return {0.7514, 0.1782, 0.0704}; 
        // Blind spot fallback
        else return {0.0, 0.0, 1.0}; 
    }

    // === MODIFIED FUZZY LOGIC (9 RULES) ===
    double re_low = FuzzyTriangle(re, -0.1, 0.0, 0.4);
    double re_med = FuzzyTriangle(re, 0.2, 0.5, 0.8);
    double re_high = FuzzyTriangle(re, 0.6, 1.0, 1.1);
    
    double cd_busy = FuzzyTriangle(cd_score, -0.1, 0.0, 0.4); 
    double cd_normal = FuzzyTriangle(cd_score, 0.2, 0.5, 0.8);
    double cd_free = FuzzyTriangle(cd_score, 0.6, 1.0, 1.1);

    double w_cd_num = 0.0, w_re_num = 0.0, w_hc_num = 0.0, total_fire = 0.0;
    auto AddRule = [&](double fireStrength, double out_cd, double out_re, double out_hc) {
        w_cd_num += fireStrength * out_cd; w_re_num += fireStrength * out_re; w_hc_num 
        += fireStrength * out_hc; total_fire += fireStrength;
    };

    AddRule(std::min(re_low, cd_busy), 0.45, 0.50, 0.05);
    AddRule(std::min(re_low, cd_normal), 0.20, 0.70, 0.10);
    AddRule(std::min(re_low, cd_free), 0.10, 0.80, 0.10);
    AddRule(std::min(re_med, cd_busy), 0.70, 0.20, 0.10);
    AddRule(std::min(re_med, cd_normal), 0.33, 0.34, 0.33);
    AddRule(std::min(re_med, cd_free), 0.20, 0.20, 0.60);
    AddRule(std::min(re_high, cd_busy), 0.80, 0.10, 0.10);
    AddRule(std::min(re_high, cd_normal), 0.20, 0.10, 0.70);
    AddRule(std::min(re_high, cd_free), 0.10, 0.05, 0.85);

    if (total_fire == 0) return {0.333, 0.333, 0.333};
    return {w_cd_num / total_fire, w_re_num / total_fire, w_hc_num / total_fire};
}

void RoutingProtocol::SelectBestEocwPath(uint32_t rreqId, Ipv4Address origin, Ipv4Address destination)
{
    auto it = m_eocwPathCache.find(rreqId);
    if (it == m_eocwPathCache.end() || it->second.empty()) { m_eocwPathTimers.erase(rreqId); return; }

    std::vector<EocwPath> paths = it->second;
    double currentEnergy = GetResidualEnergyScore();
    double currentCongestion = GetCongestionDegreeScore();

    std::vector<double> ahp_w = GetFuzzyWeights(currentEnergy, currentCongestion);
    std::vector<double> ewm_mu = GetEwmWeights(paths);

    double bestScore = -1.0;
    EocwPath* bestPath = nullptr;

    for (EocwPath& path : paths) {
        double score = CalculateEocwScore(path, ahp_w, ewm_mu);
        path.reverseRoute.m_pathScore = score;
        if (score > bestScore) { bestScore = score; bestPath = &path; }
    }

    if (bestPath) {
        m_seqNo++;
        RrepHeader rrepHeader(0, 0, destination, m_seqNo, origin, m_myRouteTimeout);
        rrepHeader.m_pathMinEnergy = bestPath->pathMinEnergy;
        rrepHeader.m_pathAvgCongestion = bestPath->pathAvgCongestion;
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag; tag.SetTtl(bestPath->reverseRoute.GetHop()); packet->AddPacketTag(tag);
        packet->AddHeader(rrepHeader); packet->AddHeader(TypeHeader(AODVTYPE_RREP));
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(bestPath->reverseRoute.GetInterface());
        if (socket) socket->SendTo(packet, 0, InetSocketAddress(bestPath->reverseRoute.GetNextHop(), AODV_PORT));
    }
    m_eocwPathCache.erase(rreqId); m_eocwPathTimers.erase(rreqId);
}

} // namespace aodv
} // namespace ns3