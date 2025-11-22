#include "ns3/aodv-helper.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/basic-energy-source-helper.h"
#include "ns3/wifi-radio-energy-model-helper.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h" 
#include <string>

using namespace ns3;
using namespace ns3::energy; 

NS_LOG_COMPONENT_DEFINE("AodvEocwTest");

// --- HAPUS DEFINISI KELAS MANUAL, GUNAKAN BAWAAN NS-3 ---

// --- VARIABEL TRACING ---
uint32_t g_packetsSent = 0;
uint32_t g_packetsReceived = 0;
Time g_totalDelay = Seconds(0.0);

/**
 * \brief Fungsi ini dipanggil setiap kali SERVER (Node 0) menerima paket.
 */
void OnPacketReceived(Ptr<const Packet> packet)
{
    g_packetsReceived++;
    
    TimestampTag timestamp;
    // Menggunakan TimestampTag bawaan ns-3
    if (packet->PeekPacketTag(timestamp))
    {
        Time txTime = timestamp.GetTimestamp();
        Time rxTime = Simulator::Now();
        Time delay = rxTime - txTime;
        
        if (delay.IsPositive()) {
            g_totalDelay += delay;
        }
    }
}

/**
 * \brief Fungsi ini dipanggil setiap kali KLIEN mengirim paket.
 */
void OnPacketSent(Ptr<const Packet> packet)
{
    g_packetsSent++;
    
    // 1. Buat Tag dengan waktu sekarang menggunakan TimestampTag bawaan ns-3
    TimestampTag tag;
    tag.SetTimestamp(Simulator::Now());
    
    // 2. Tempelkan tag ke paket.
    // PERBAIKAN: Menggunakan PeekPointer (Huruf P besar)
    const_cast<Packet*>(PeekPointer(packet))->AddPacketTag(tag);
}

int
main(int argc, char* argv[])
{
    // --- Konfigurasi Simulasi ---
    uint32_t numNodes = 40;    
    double simTime = 200.0;    
    double nodeSpeed = 5.0;    
    double initialEnergy = 50.0; 
    double arenaX = 100.0;     
    double arenaY = 100.0;     

    CommandLine cmd(__FILE__);
    cmd.AddValue("numNodes", "Jumlah node", numNodes);
    cmd.AddValue("simTime", "Waktu simulasi (detik)", simTime);
    cmd.AddValue("nodeSpeed", "Kecepatan node (m/s)", nodeSpeed);
    cmd.AddValue("initialEnergy", "Energi awal (J)", initialEnergy);
    cmd.Parse(argc, argv);

    // --- Node & Kontainer ---
    NodeContainer nodes;
    nodes.Create(numNodes);

    // --- Fisik & Saluran WIFI ---
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    // --- Model Energi ---
    BasicEnergySourceHelper basicEnergySourceHelper;
    basicEnergySourceHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(initialEnergy));
    EnergySourceContainer energySources = basicEnergySourceHelper.Install(nodes);

    WifiRadioEnergyModelHelper radioEnergyModelHelper;
    radioEnergyModelHelper.Set("TxCurrentA", DoubleValue(0.038)); 
    radioEnergyModelHelper.Set("RxCurrentA", DoubleValue(0.031)); 

    // --- Konfigurasi MAC & NetDevice ---
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac"); 
    Ssid ssid = Ssid("ns3-aodv-test");
    WifiHelper wifi;
    
    // Gunakan standar 802.11b agar jangkauan lebih jauh/stabil
    wifi.SetStandard(WIFI_STANDARD_80211b);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("DsssRate1Mbps"),
                                 "ControlMode", StringValue("DsssRate1Mbps"));

    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // --- Mobilitas ---
    MobilityHelper mobility;
    Ptr<RandomRectanglePositionAllocator> positionAlloc = CreateObject<RandomRectanglePositionAllocator>();

    Ptr<UniformRandomVariable> xRandom = CreateObject<UniformRandomVariable>();
    xRandom->SetAttribute("Min", DoubleValue(0.0));
    xRandom->SetAttribute("Max", DoubleValue(arenaX));
    positionAlloc->SetX(xRandom);

    Ptr<UniformRandomVariable> yRandom = CreateObject<UniformRandomVariable>();
    yRandom->SetAttribute("Min", DoubleValue(0.0));
    yRandom->SetAttribute("Max", DoubleValue(arenaY));
    positionAlloc->SetY(yRandom); 
    
    mobility.SetPositionAllocator(positionAlloc);

    std::string speedString = "ns3::ConstantRandomVariable[Constant=" + std::to_string(nodeSpeed) + "]";
    std::string pauseString = "ns3::ConstantRandomVariable[Constant=1.0]";

    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                              "Speed", StringValue(speedString),
                              "Pause", StringValue(pauseString),
                              "PositionAllocator", PointerValue(positionAlloc));

    mobility.Install(nodes);

    // --- Stack Internet (AODV) ---
    AodvHelper aodv; 
    aodv.Set("EnableHello", BooleanValue(false)); 
    aodv.Set("DestinationOnly", BooleanValue(true));
    InternetStackHelper stack;
    stack.SetRoutingHelper(aodv); 
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // --- Aplikasi (UDP Echo) ---
    
    // 1. Server di node 0
    UdpEchoServerHelper echoServer(9); 
    ApplicationContainer serverApps = echoServer.Install(nodes.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simTime));
    
    // Trace RX di server
    serverApps.Get(0)->TraceConnectWithoutContext("Rx", MakeCallback(&OnPacketReceived));

    // 2. Klien di node terakhir
    UdpEchoClientHelper echoClient(interfaces.GetAddress(0), 9); 
    echoClient.SetAttribute("MaxPackets", UintegerValue(50));
    echoClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
    echoClient.SetAttribute("PacketSize", UintegerValue(1024));

    ApplicationContainer clientApps = echoClient.Install(nodes.Get(numNodes - 1));
    clientApps.Start(Seconds(2.01));
    clientApps.Stop(Seconds(simTime));
    
    // Trace TX di klien
    clientApps.Get(0)->TraceConnectWithoutContext("Tx", MakeCallback(&OnPacketSent));

    // --- Logging & Animasi ---
    LogComponentEnable("AodvRoutingProtocol", LOG_LEVEL_INFO);

    AnimationInterface anim("aodv-eocw-animation.xml");
    anim.SetMobilityPollInterval(Seconds(1)); 
    anim.EnablePacketMetadata(true);

    // --- Menjalankan Simulasi ---
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    // --- CETAK HASIL STATISTIK ---
    double pdr = 0.0;
    if (g_packetsSent > 0)
    {
        pdr = ((double)g_packetsReceived / (double)g_packetsSent) * 100.0;
    }

    double avgDelay = 0.0;
    if (g_packetsReceived > 0)
    {
        avgDelay = g_totalDelay.GetMilliSeconds() / (double)g_packetsReceived;
    }

    double shutdownThreshold = 0.1; 
    uint32_t liveNodes = 0;
    for (uint32_t i = 0; i < energySources.GetN(); ++i)
    {
        if (energySources.Get(i)->GetRemainingEnergy() > shutdownThreshold)
        {
            liveNodes++;
        }
    }
    double survivalRate = ((double)liveNodes / (double)numNodes) * 100.0;

    std::cout << "--- HASIL SIMULASI (AODV-EOCW) ---" << std::endl;
    std::cout << " Waktu Simulasi:   " << simTime << " detik" << std::endl;
    std::cout << " Jumlah Node:      " << numNodes << std::endl;
    std::cout << " Kecepatan Node:   " << nodeSpeed << " m/s" << std::endl;
    std::cout << " Area Simulasi:    " << arenaX << "m x " << arenaY << "m" << std::endl;
    std::cout << " Energi Awal:      " << initialEnergy << " J" << std::endl;
    std::cout << " ----------------------------------" << std::endl;
    std::cout << " Total Paket Terkirim: " << g_packetsSent << std::endl;
    std::cout << " Total Paket Diterima: " << g_packetsReceived << std::endl;
    std::cout << " Packet Delivery Rate (PDR): " << pdr << " %" << std::endl;
    std::cout << " Average End-to-End Delay: " << avgDelay << " ms" << std::endl;
    std::cout << " Node Hidup:                 " << liveNodes << " / " << numNodes << std::endl;
    std::cout << " Survival Rate:              " << survivalRate << " %" << std::endl;
    std::cout << " ----------------------------------" << std::endl;

    return 0;
}