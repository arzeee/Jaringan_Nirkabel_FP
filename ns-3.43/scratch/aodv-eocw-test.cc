/*
 * AODV-EOCW Stress Test Simulation (Randomized Energy)
 * * Tujuan: Memberikan variasi energi awal (Heterogen) agar hasil survival rate
 * * lebih realistis dan bisa dibandingkan (tidak 0% atau 100%).
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
#include "ns3/wifi-radio-energy-model-helper.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor.h"
#include "ns3/ipv4-flow-classifier.h"
#include <string>
#include <vector>

using namespace ns3;
using namespace ns3::energy;

NS_LOG_COMPONENT_DEFINE("AodvEocwStressTest");

// --- GLOBAL VARIABLES ---
std::vector<bool> isNodeDead;
std::vector<double> nodeRealEnergyLimit; // Batas energi unik tiap node
double FAKE_INITIAL_ENERGY = 1000000.0;  // Energi tak terbatas untuk PHY

// Fungsi Soft Kill
void SoftKillNode(Ptr<Node> node)
{
    uint32_t id = node->GetId();
    if (isNodeDead[id]) return; 

    NS_LOG_UNCOND("DEBUG: Node " << id << " Mati (Batas Energi " << nodeRealEnergyLimit[id] << "J Tercapai).");
    
    isNodeDead[id] = true; 

    // Matikan App
    for (uint32_t i = 0; i < node->GetNApplications(); ++i)
    {
        node->GetApplication(i)->SetStopTime(Simulator::Now());
    }

    // Matikan IPv4
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (ipv4 && ipv4->GetNInterfaces() > 1)
    {
        ipv4->SetDown(1); 
    }
}

// Handler Energi Real-time
void EnergyChangeHandler(Ptr<Node> node, double oldValue, double newValue)
{
    uint32_t id = node->GetId();
    double consumed = FAKE_INITIAL_ENERGY - newValue;

    // Cek terhadap batas energi KHUSUS node ini
    if (consumed >= nodeRealEnergyLimit[id] && !isNodeDead[id])
    {
        SoftKillNode(node);
    }
}

int main(int argc, char* argv[])
{
    uint32_t numNodes = 50;
    double simTime = 100.0;
    double nodeSpeed = 10.0;
    
    // Range Energi Acak (Min - Max)
    double minEnergy = 60.0; 
    double maxEnergy = 100.0;

    double arenaX = 500.0;
    double arenaY = 500.0;
    uint32_t numFlows = 5;
    std::string protocolName = "AODV-EOCW"; 

    CommandLine cmd(__FILE__);
    cmd.AddValue("numNodes", "Jumlah node", numNodes);
    cmd.AddValue("simTime", "Waktu simulasi (detik)", simTime);
    cmd.AddValue("numFlows", "Jumlah aliran trafik (UDP)", numFlows);
    cmd.AddValue("minEnergy", "Energi minimal acak (Joule)", minEnergy);
    cmd.AddValue("maxEnergy", "Energi maksimal acak (Joule)", maxEnergy);
    cmd.Parse(argc, argv);

    isNodeDead.assign(numNodes, false);

    // --- GENERATE RANDOM ENERGY LIMITS ---
    // Agar simulasi lebih realistis, setiap node punya baterai beda-beda
    Ptr<UniformRandomVariable> energyRng = CreateObject<UniformRandomVariable>();
    energyRng->SetAttribute("Min", DoubleValue(minEnergy));
    energyRng->SetAttribute("Max", DoubleValue(maxEnergy));
    
    // Set seed agar random-nya konsisten (reproducible) tiap kali run
    energyRng->SetStream(1); 

    for(uint32_t i=0; i<numNodes; i++) {
        nodeRealEnergyLimit.push_back(energyRng->GetValue());
    }

    // --- SETUP NODE & WIFI (SAMA SEPERTI SEBELUMNYA) ---
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

    BasicEnergySourceHelper basicEnergySourceHelper;
    basicEnergySourceHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(FAKE_INITIAL_ENERGY));
    EnergySourceContainer energySources = basicEnergySourceHelper.Install(nodes);

    WifiRadioEnergyModelHelper radioEnergyModelHelper;
    radioEnergyModelHelper.Set("TxCurrentA", DoubleValue(0.0174)); 
    radioEnergyModelHelper.Set("RxCurrentA", DoubleValue(0.0197));
    radioEnergyModelHelper.Install(devices, energySources);

    for (uint32_t i = 0; i < numNodes; i++)
    {
        Ptr<BasicEnergySource> basicSource = DynamicCast<BasicEnergySource>(energySources.Get(i));
        basicSource->TraceConnectWithoutContext("RemainingEnergy", MakeBoundCallback(&EnergyChangeHandler, nodes.Get(i)));
    }

    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::RandomRectanglePositionAllocator",
                                  "X", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(arenaX) + "]"),
                                  "Y", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(arenaY) + "]"));
    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                              "Speed", StringValue("ns3::UniformRandomVariable[Min=0.0|Max=" + std::to_string(nodeSpeed) + "]"),
                              "Pause", StringValue("ns3::ConstantRandomVariable[Constant=2.0]"),
                              "PositionAllocator", PointerValue(CreateObject<RandomRectanglePositionAllocator>()));
    mobility.Install(nodes);

    AodvHelper aodv;
    aodv.Set("DestinationOnly", BooleanValue(true)); 
    aodv.Set("EnableHello", BooleanValue(true)); 
    InternetStackHelper stack;
    stack.SetRoutingHelper(aodv);
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    uint16_t port = 9;
    Ptr<UniformRandomVariable> rnd = CreateObject<UniformRandomVariable>();
    rnd->SetAttribute("Min", DoubleValue(0.0));
    rnd->SetAttribute("Max", DoubleValue(numNodes - 1));
    ApplicationContainer clientApps, serverApps;

    for (uint32_t i = 0; i < numFlows; ++i)
    {
        uint32_t srcNode = (uint32_t)rnd->GetValue();
        uint32_t dstNode = (uint32_t)rnd->GetValue();
        while (srcNode == dstNode) dstNode = (uint32_t)rnd->GetValue();

        UdpEchoServerHelper server(port);
        serverApps.Add(server.Install(nodes.Get(dstNode)));
        UdpEchoClientHelper client(interfaces.GetAddress(dstNode), port);
        client.SetAttribute("MaxPackets", UintegerValue(100)); 
        client.SetAttribute("Interval", TimeValue(Seconds(0.5))); 
        client.SetAttribute("PacketSize", UintegerValue(512)); 
        
        Time startTime = Seconds(1.0 + i * 2.0); 
        clientApps.Add(client.Install(nodes.Get(srcNode)));
        clientApps.Get(i)->SetStartTime(startTime);
        clientApps.Get(i)->SetStopTime(Seconds(simTime - 1.0));
        port++;
    }
    
    serverApps.Start(Seconds(0.5));
    serverApps.Stop(Seconds(simTime));

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();
    LogComponentEnable("AodvRoutingProtocol", LOG_LEVEL_INFO);

    NS_LOG_INFO("Memulai Simulasi " << protocolName << " (Random Energy " << minEnergy << "-" << maxEnergy << "J)...");

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();

    double totalTx = 0, totalRx = 0, totalDelay = 0, totalThroughput = 0;
    for (auto i = stats.begin(); i != stats.end(); ++i)
    {
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
    for (uint32_t i = 0; i < numNodes; ++i)
    {
        double fakeRemaining = energySources.Get(i)->GetRemainingEnergy();
        double consumed = FAKE_INITIAL_ENERGY - fakeRemaining;
        
        if (isNodeDead[i]) {
            deadNodes++;
            totalConsumedEnergy += nodeRealEnergyLimit[i]; // Hitung full limit
        } else {
            totalConsumedEnergy += consumed;
        }
    }

    double avgPdr = (totalTx > 0) ? (totalRx / totalTx) * 100.0 : 0.0;
    double avgDelayMs = (totalRx > 0) ? (totalDelay / totalRx) * 1000.0 : 0.0;
    double survivalRate = ((double)(numNodes - deadNodes) / numNodes) * 100.0;

    std::cout << "\n============ HASIL AKHIR ============" << std::endl;
    std::cout << "Range Energi:         " << minEnergy << " - " << maxEnergy << " J" << std::endl;
    std::cout << "PDR (Delivery Rate):  " << avgPdr << " %" << std::endl;
    std::cout << "Survival Rate:        " << survivalRate << " % (" << deadNodes << " mati)" << std::endl;
    std::cout << "Rata-rata Delay:      " << avgDelayMs << " ms" << std::endl;
    std::cout << "Total Throughput:     " << totalThroughput << " Kbps" << std::endl;
    std::cout << "Total Energi Terpakai:" << totalConsumedEnergy << " J" << std::endl;
    std::cout << "=====================================" << std::endl;

    Simulator::Destroy();
    return 0;
}