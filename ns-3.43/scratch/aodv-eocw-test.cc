/* aodv-eocw-test.cc
 *
 * AODV-EOCW Final Test (Intermittent Traffic Strategy)
 * Status: STABLE FIXED (Override energy-depletion callback to avoid WifiPhy::SetOffMode)
 *
 * Usage:
 *   ./ns3 run "scratch/aodv-eocw-test --useFuzzy=true"
 *
 * Notes:
 * - This version targets ns-3.43 (CMake build).
 * - The energy-depletion callback is set via MakeBoundCallback(...) so the bound Node*
 *   is passed to the handler; we then call your SoftKillNode(node).
 */

#include "ns3/aodv-helper.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/basic-energy-source-helper.h"
#include "ns3/basic-energy-source.h"
#include "ns3/energy-source.h"
#include "ns3/wifi-radio-energy-model-helper.h"
#include "ns3/wifi-radio-energy-model.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/on-off-helper.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/wifi-net-device.h"
#include "ns3/adhoc-wifi-mac.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/qos-utils.h"
#include "ns3/device-energy-model.h"
#include <string>
#include <vector>

using namespace ns3;
using namespace ns3::energy;

NS_LOG_COMPONENT_DEFINE("AodvEocwStressTest");

// --- GLOBAL VARIABLES ---
std::vector<bool> isNodeDead;

// Forward declarations
void SoftKillNode(Ptr<Node> node);

// Soft-kill a node safely: flush MAC TX queues, stop apps, and set IPv4 interface down
void SoftKillNode(Ptr<Node> node)
{
    if (!node) return;
    uint32_t id = node->GetId();
    if (id < isNodeDead.size() && isNodeDead[id]) return;

    // Log to console for debug
    NS_LOG_UNCOND("!!! NODE " << id << " DIED (Energy Depleted) at " << Simulator::Now().GetSeconds() << "s !!!");

    if (id < isNodeDead.size()) isNodeDead[id] = true;

    // 1. Flush Queues (so queued packets won't be transmitted)
    for (uint32_t i = 0; i < node->GetNDevices(); ++i) {
        Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(node->GetDevice(i));
        if (wifiDev) {
            Ptr<WifiMac> mac = wifiDev->GetMac();
            if (mac) {
                Ptr<AdhocWifiMac> adhocMac = DynamicCast<AdhocWifiMac>(mac);
                if (adhocMac) {
                    for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
                        Ptr<WifiMacQueue> queue = adhocMac->GetTxopQueue(ac);
                        if (queue) queue->Flush();
                    }
                }
            }
            // IMPORTANT: Do NOT call phy->SetOffMode() here. That can produce invalid state transitions if PHY busy.
        }
    }

    // 2. Stop Applications immediately
    for (uint32_t i = 0; i < node->GetNApplications(); ++i) {
        Ptr<Application> app = node->GetApplication(i);
        if (app) {
            app->SetStopTime(Simulator::Now());
        }
    }

    // 3. Bring down IPv4 interface to notify routing layer (AODV) of link break
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    // Typically interface 1 is the wifi interface in this script; ensure index valid
    if (ipv4 && ipv4->GetNInterfaces() > 1) {
        ipv4->SetDown(1);
    }
}

// Bound depletion callback: will be created with MakeBoundCallback(&WifiEnergyDepletionBoundCallback, nodePtr)
// Signature must match function pointer type used by MakeBoundCallback (i.e., void(Ptr<Node>))
void WifiEnergyDepletionBoundCallback(Ptr<Node> node)
{
    // Call your soft-kill routine safely
    SoftKillNode(node);
}

// Callback automatically called when energy value trace fires (optional extra safeguard)
void EnergyChangeHandler(Ptr<Node> node, double oldValue, double newValue)
{
    // trigger soft kill slightly before absolute zero
    if (newValue <= 0.1) {
        Simulator::ScheduleNow(&SoftKillNode, node);
    }
}

int main(int argc, char* argv[])
{
    bool useFuzzy = true;
    uint32_t numNodes = 40;
    double simTime = 200.0;
    double nodeSpeed = 10.0;
    double arenaSize = 1000.0;

    // Range Energi (Dibuat rendah agar node cepat mati untuk pengujian)
    double minEnergy = 0.1;
    double maxEnergy = 0.3;
    uint32_t numFlows = 5;

    CommandLine cmd(__FILE__);
    cmd.AddValue("useFuzzy", "Use Fuzzy Logic", useFuzzy);
    cmd.AddValue("numNodes", "Number of Nodes", numNodes);
    // --- TAMBAHKAN BARIS-BARIS INI ---
    cmd.AddValue("simTime", "Simulation Time", simTime);
    cmd.AddValue("speed", "Node Speed", nodeSpeed);
    cmd.AddValue("energyMin", "Min Energy", minEnergy);
    cmd.AddValue("energyMax", "Max Energy", maxEnergy);
    // ---------------------------------
    cmd.Parse(argc, argv);

    // Timeout pendek agar RREQ sering dikirim ulang (memicu logika EOCW bekerja lebih sering)
    Config::SetDefault("ns3::aodv::RoutingProtocol::ActiveRouteTimeout", TimeValue(Seconds(3.0)));

    isNodeDead.assign(numNodes, false);

    NodeContainer nodes;
    nodes.Create(numNodes);

    Ptr<UniformRandomVariable> energyRng = CreateObject<UniformRandomVariable>();
    energyRng->SetAttribute("Min", DoubleValue(minEnergy));
    energyRng->SetAttribute("Max", DoubleValue(maxEnergy));
    energyRng->SetStream(1);

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("ErpOfdmRate6Mbps"),
                                 "ControlMode", StringValue("ErpOfdmRate6Mbps"));
    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::RandomRectanglePositionAllocator",
                                  "X", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(arenaSize) + "]"),
                                  "Y", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(arenaSize) + "]"));
    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                              "Speed", StringValue("ns3::UniformRandomVariable[Min=" + std::to_string(nodeSpeed - 1.0) + "|Max=" + std::to_string(nodeSpeed + 1.0) + "]"),
                              "Pause", StringValue("ns3::ConstantRandomVariable[Constant=0.0]"),
                              "PositionAllocator", PointerValue(CreateObject<RandomRectanglePositionAllocator>()));
    mobility.Install(nodes);

    AodvHelper aodv;
    aodv.Set("DestinationOnly", BooleanValue(true));
    aodv.Set("EnableFuzzy", BooleanValue(useFuzzy));
    InternetStackHelper stack;
    stack.SetRoutingHelper(aodv);
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // --- INSTALASI ENERGI ---
    BasicEnergySourceHelper basicSourceHelper;
    EnergySourceContainer sources = basicSourceHelper.Install(nodes);

    WifiRadioEnergyModelHelper radioEnergyModelHelper;

    // 1. Idle Current KECIL (Agar node tidak mati kalau diam saja)
    // 0.001 A = Sangat irit. Node bisa hidup lama jika tidak dipakai.
    radioEnergyModelHelper.Set("IdleCurrentA", DoubleValue(0.001)); 

    // 2. Tx Current RAKSASA (Hukuman berat buat yang kirim data)
    // 2.5 A = Sekali kirim paket, baterai terkuras drastis.
    radioEnergyModelHelper.Set("TxCurrentA", DoubleValue(2.500)); 
    
    // 3. Rx Current Sedang
    radioEnergyModelHelper.Set("RxCurrentA", DoubleValue(0.500));

    // Install returns a DeviceEnergyModelContainer (one entry per device energy model installed)
    DeviceEnergyModelContainer demContainer = radioEnergyModelHelper.Install(devices, sources);

    // IMPORTANT: override per-model depletion callback using MakeBoundCallback to bind the corresponding node pointer
    // The mapping is: sources.Get(i) corresponds to nodes.Get(i) if you installed sources with basicSourceHelper.Install(nodes)
    // We'll iterate by node index and try to set callbacks on all WifiRadioEnergyModel instances attached to that node's source.
    for (uint32_t nodeIndex = 0; nodeIndex < nodes.GetN(); ++nodeIndex) {
        Ptr<BasicEnergySource> src = DynamicCast<BasicEnergySource>(sources.Get(nodeIndex));
        if (!src) continue;

        // Find all WifiRadioEnergyModel device models attached to this source
        DeviceEnergyModelContainer devModels = src->FindDeviceEnergyModels("ns3::WifiRadioEnergyModel");
        for (uint32_t k = 0; k < devModels.GetN(); ++k) {
            Ptr<DeviceEnergyModel> dem = devModels.Get(k);
            if (!dem) continue;

            Ptr<WifiRadioEnergyModel> wrem = DynamicCast<WifiRadioEnergyModel>(dem);
            if (!wrem) continue;

            // Bind the node pointer to the depletion callback.
            // MakeBoundCallback builds a Callback<void> by binding the Ptr<Node> argument.
            wrem->SetEnergyDepletionCallback(MakeBoundCallback(&WifiEnergyDepletionBoundCallback, nodes.Get(nodeIndex)));
        }
    }

    // Also optionally set RemainingEnergy trace handlers so we get notifications while running
    for (uint32_t i = 0; i < numNodes; i++) {
        Ptr<BasicEnergySource> s = DynamicCast<BasicEnergySource>(sources.Get(i));
        if (!s) continue;
        // Set energi awal secara random
        s->SetInitialEnergy(energyRng->GetValue());
        // Pasang callback untuk memantau energi secara real-time (optional)
        s->TraceConnectWithoutContext("RemainingEnergy", MakeBoundCallback(&EnergyChangeHandler, nodes.Get(i)));
    }

    // --- TRAFFIC ---
    uint16_t port = 9;
    Ptr<UniformRandomVariable> nodeRng = CreateObject<UniformRandomVariable>();
    nodeRng->SetAttribute("Min", DoubleValue(0.0));
    nodeRng->SetAttribute("Max", DoubleValue(numNodes - 1));
    nodeRng->SetStream(2);

    OnOffHelper onoff("ns3::UdpSocketFactory", Address());
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=5.0]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=5.0]"));
    onoff.SetAttribute("DataRate", StringValue("64kbps"));
    onoff.SetAttribute("PacketSize", UintegerValue(256));

    ApplicationContainer apps;
    for (uint32_t i = 0; i < numFlows; ++i) {
        uint32_t srcIdx = (uint32_t)nodeRng->GetValue();
        uint32_t dstIdx = (uint32_t)nodeRng->GetValue();
        while (srcIdx == dstIdx) dstIdx = (uint32_t)nodeRng->GetValue();

        PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        apps.Add(sink.Install(nodes.Get(dstIdx)));

        AddressValue remoteAddress(InetSocketAddress(interfaces.GetAddress(dstIdx), port));
        onoff.SetAttribute("Remote", remoteAddress);
        apps.Add(onoff.Install(nodes.Get(srcIdx)));

        apps.Get(apps.GetN() - 1)->SetStartTime(Seconds(1.0 + i));
        apps.Get(apps.GetN() - 1)->SetStopTime(Seconds(simTime - 2.0));
        port++;
    }

    apps.Start(Seconds(0.5));

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Stats Analysis
    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
    double totalRx = 0, totalTx = 0, totalDelay = 0, totalThroughput = 0;

    for (auto i = stats.begin(); i != stats.end(); ++i) {
        totalTx += i->second.txPackets;
        totalRx += i->second.rxPackets;
        if (i->second.rxPackets > 0) {
            totalDelay += i->second.delaySum.GetSeconds();
            double duration = i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds();
            if (duration > 0) totalThroughput += (i->second.rxBytes * 8.0) / duration / 1024.0;
        }
    }

    double totalConsumed = 0;
    uint32_t deadCount = 0;
    for (uint32_t i = 0; i < numNodes; i++) {
        Ptr<BasicEnergySource> s = DynamicCast<BasicEnergySource>(sources.Get(i));
        if (!s) continue;
        double consumed = s->GetInitialEnergy() - s->GetRemainingEnergy();
        if (i < isNodeDead.size() && isNodeDead[i]) {
            deadCount++;
            // Jika mati, anggap semua energi awal telah habis dikonsumsi (untuk perbandingan yang adil)
            consumed = s->GetInitialEnergy();
        }
        totalConsumed += consumed;
    }

    double avgPdr = (totalTx > 0) ? (totalRx / totalTx) * 100.0 : 0.0;
    double avgDelay = (totalRx > 0) ? (totalDelay / totalRx) * 1000.0 : 0.0;
    double avgThroughput = (stats.size() > 0) ? totalThroughput : 0.0;
    double survival = ((double)(numNodes - deadCount) / numNodes) * 100.0;

    // Output CSV
    std::cout << (useFuzzy ? "Modified_Fuzzy" : "Original_Paper") << ","
              << nodeSpeed << ","
              << numNodes << ","
              << avgPdr << ","
              << avgDelay << ","
              << survival << ","
              << totalConsumed << ","
              << avgThroughput << std::endl;

    Simulator::Destroy();
    return 0;
}
