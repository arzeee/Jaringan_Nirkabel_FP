/*
 * AODV-EOCW Simulation (Safe Virtual Battery Strategy)
 * * Status: COMPILER-SAFE & CRASH-FREE.
 * * Metode: Memberikan energi tak terbatas ke NS-3 (Anti-Crash),
 * * lalu mematikan logika Network saat batas energi 'Real' tercapai.
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
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/on-off-helper.h"
#include "ns3/packet-sink-helper.h"
#include <string>
#include <vector>

using namespace ns3;
using namespace ns3::energy;

NS_LOG_COMPONENT_DEFINE("AodvEocwStressTest");

// --- GLOBAL VARIABLES ---
std::vector<bool> isNodeDead;
std::vector<double> nodeRealEnergyLimit; // Batas energi 'Real'
double FAKE_INITIAL_ENERGY = 1000000000.0; // 1 Milyar Joule (Infinite)

// Fungsi Mematikan Logika Node (Soft Kill)
void SoftKillNode(Ptr<Node> node)
{
    uint32_t id = node->GetId();
    if (isNodeDead[id]) return; 

    // NS_LOG_UNCOND("DEBUG: Node " << id << " Mati Kehabisan Energi (Limit Asli Tercapai).");
    
    isNodeDead[id] = true; 

    // 1. Hentikan Aplikasi
    for (uint32_t i = 0; i < node->GetNApplications(); ++i) {
        node->GetApplication(i)->SetStopTime(Simulator::Now());
    }

    // 2. Putuskan Koneksi IPv4
    // Node akan berhenti mengirim/menerima paket routing.
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (ipv4 && ipv4->GetNInterfaces() > 1) {
        ipv4->SetDown(1); 
    }
    
    // PENTING: Radio PHY tidak disentuh, tetap hidup (Zombie) supaya tidak Crash.
}

// Handler Perubahan Energi
void EnergyChangeHandler(Ptr<Node> node, double oldValue, double newValue)
{
    // Hitung konsumsi real
    double consumed = FAKE_INITIAL_ENERGY - newValue;
    uint32_t id = node->GetId();

    // Jika konsumsi melebihi batas 'Real' yang kita tentukan -> Matikan Node
    if (consumed >= nodeRealEnergyLimit[id] && !isNodeDead[id])
    {
        SoftKillNode(node);
    }
}

int main(int argc, char* argv[])
{
    bool useFuzzy = true;
    uint32_t numNodes = 30;
    double simTime = 200.0;
    double nodeSpeed = 10.0;
    double arenaSize = 1000.0;
    
    // Range Energi Kritis (15J - 35J) agar Fuzzy Logic "panik"
    double minEnergy = 15.0; 
    double maxEnergy = 35.0;
    
    uint32_t numFlows = 10;

    CommandLine cmd(__FILE__);
    cmd.AddValue("useFuzzy", "Use Fuzzy Logic", useFuzzy);
    cmd.AddValue("numNodes", "Number of Nodes", numNodes);
    cmd.AddValue("speed", "Node Speed", nodeSpeed);
    cmd.AddValue("simTime", "Simulation Time", simTime);
    cmd.AddValue("energyMin", "Min Random Energy", minEnergy);
    cmd.AddValue("energyMax", "Max Random Energy", maxEnergy);
    cmd.Parse(argc, argv);

    // Timeout Pendek agar sering Re-Routing
    Config::SetDefault("ns3::aodv::RoutingProtocol::ActiveRouteTimeout", TimeValue(Seconds(3.0)));

    isNodeDead.assign(numNodes, false);
    
    // Generate Batas Energi 'Real'
    Ptr<UniformRandomVariable> energyRng = CreateObject<UniformRandomVariable>();
    energyRng->SetAttribute("Min", DoubleValue(minEnergy));
    energyRng->SetAttribute("Max", DoubleValue(maxEnergy));
    energyRng->SetStream(1); 
    for(uint32_t i=0; i<numNodes; i++) nodeRealEnergyLimit.push_back(energyRng->GetValue());

    NodeContainer nodes;
    nodes.Create(numNodes);

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager", "DataMode", StringValue("ErpOfdmRate6Mbps"), "ControlMode", StringValue("ErpOfdmRate6Mbps"));
    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // INSTALL ENERGI PALSU (INFINITE)
    BasicEnergySourceHelper basicEnergySourceHelper;
    // Pasang 1 Milyar Joule ke simulator
    basicEnergySourceHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(FAKE_INITIAL_ENERGY));
    EnergySourceContainer energySources = basicEnergySourceHelper.Install(nodes);

    WifiRadioEnergyModelHelper radioEnergyModelHelper;
    radioEnergyModelHelper.Set("TxCurrentA", DoubleValue(0.0174)); 
    radioEnergyModelHelper.Set("RxCurrentA", DoubleValue(0.0197));
    radioEnergyModelHelper.Install(devices, energySources);

    // PASANG MONITOR
    for (uint32_t i = 0; i < numNodes; i++) {
        // Kita casting ke BasicEnergySource agar TraceConnect aman
        Ptr<BasicEnergySource> es = DynamicCast<BasicEnergySource>(energySources.Get(i));
        es->TraceConnectWithoutContext("RemainingEnergy", MakeBoundCallback(&EnergyChangeHandler, nodes.Get(i)));
        
        // Kita TIDAK memanggil SetEnergyDepletionCallback lagi (Sumber masalah kompilasi)
    }

    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::RandomRectanglePositionAllocator",
                                  "X", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(arenaSize) + "]"),
                                  "Y", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(arenaSize) + "]"));
    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                              "Speed", StringValue("ns3::UniformRandomVariable[Min=" + std::to_string(nodeSpeed-1.0) + "|Max=" + std::to_string(nodeSpeed+1.0) + "]"),
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

    // TRAFIK ON/OFF (Intermittent)
    uint16_t port = 9;
    Ptr<UniformRandomVariable> rnd = CreateObject<UniformRandomVariable>();
    rnd->SetAttribute("Min", DoubleValue(0.0));
    rnd->SetAttribute("Max", DoubleValue(numNodes - 1));
    rnd->SetStream(2); 

    OnOffHelper onoff("ns3::UdpSocketFactory", Address());
    // Kirim 5 detik, Diam 5 detik -> Memicu Re-Routing
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=5.0]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=5.0]"));
    onoff.SetAttribute("DataRate", StringValue("64kbps")); 
    onoff.SetAttribute("PacketSize", UintegerValue(512));

    ApplicationContainer clientApps, serverApps;

    for (uint32_t i = 0; i < numFlows; ++i)
    {
        uint32_t src = (uint32_t)rnd->GetValue();
        uint32_t dst = (uint32_t)rnd->GetValue();
        while (src == dst) dst = (uint32_t)rnd->GetValue();

        PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        serverApps.Add(sink.Install(nodes.Get(dst)));

        AddressValue remoteAddress(InetSocketAddress(interfaces.GetAddress(dst), port));
        onoff.SetAttribute("Remote", remoteAddress);
        clientApps.Add(onoff.Install(nodes.Get(src)));
        
        double startT = 1.0 + i * 1.0; 
        clientApps.Get(i)->SetStartTime(Seconds(startT));
        clientApps.Get(i)->SetStopTime(Seconds(simTime - 2.0));
        
        port++;
    }
    serverApps.Start(Seconds(0.5));

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();
    
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
    double totalTx = 0, totalRx = 0, totalDelay = 0, totalThroughput = 0;
    for (auto i = stats.begin(); i != stats.end(); ++i) {
        totalTx += i->second.txPackets;
        totalRx += i->second.rxPackets;
        if (i->second.rxPackets > 0) {
            totalDelay += i->second.delaySum.GetSeconds();
            double duration = i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds();
            if (duration > 0) totalThroughput += (i->second.rxBytes * 8.0) / duration / 1024.0;
        }
    }

    double totalConsumedEnergy = 0;
    uint32_t deadNodes = 0;
    for (uint32_t i = 0; i < numNodes; ++i) {
        // Ambil sisa energi palsu
        double fakeRemaining = energySources.Get(i)->GetRemainingEnergy();
        // Hitung konsumsi
        double consumed = FAKE_INITIAL_ENERGY - fakeRemaining;
        
        // Cek terhadap limit REAL
        if (isNodeDead[i] || consumed >= nodeRealEnergyLimit[i]) {
            deadNodes++;
            totalConsumedEnergy += nodeRealEnergyLimit[i]; // Max energy (habis)
        } else {
            totalConsumedEnergy += consumed;
        }
    }

    double avgPdr = (totalTx > 0) ? (totalRx / totalTx) * 100.0 : 0.0;
    double avgDelayMs = (totalRx > 0) ? (totalDelay / totalRx) * 1000.0 : 0.0;
    double avgThroughput = (stats.size() > 0) ? totalThroughput : 0.0; 
    double survivalRate = ((double)(numNodes - deadNodes) / numNodes) * 100.0;

    std::cout << (useFuzzy ? "Modified_Fuzzy" : "Original_Paper") << ","
              << nodeSpeed << ","
              << numNodes << ","
              << avgPdr << ","
              << avgDelayMs << ","
              << survivalRate << ","
              << totalConsumedEnergy << ","
              << avgThroughput << std::endl;

    Simulator::Destroy();
    return 0;
}