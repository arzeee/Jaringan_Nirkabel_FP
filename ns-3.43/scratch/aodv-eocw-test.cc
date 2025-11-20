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
#include "ns3/netanim-module.h" // <-- Include NetAnim
#include "ns3/network-module.h" // <-- TAMBAHKAN BARIS INI
#include <string> // <-- DIPERLUKAN UNTUK std::to_string

using namespace ns3;
using namespace ns3::energy; 

NS_LOG_COMPONENT_DEFINE("AodvEocwTest");

// --- TAMBAHKAN VARIABEL TRACING ---
uint32_t g_packetsSent = 0;
uint32_t g_packetsReceived = 0;
Time g_totalDelay = Seconds(0.0);
// --- AKHIR TAMBAHAN ---   

/**
 * \brief Fungsi ini dipanggil setiap kali server menerima paket.
 */
void OnPacketReceived(Ptr<const Packet> packet)
{
    g_packetsReceived++;
    
    // Coba baca timestamp yang ditambahkan oleh klien
    TimestampTag timestamp;
    if (packet->PeekPacketTag(timestamp))
    {
        Time txTime = timestamp.GetTimestamp();
        Time rxTime = Simulator::Now();
        g_totalDelay += (rxTime - txTime); // Akumulasikan delay
    }
}

/**
 * \brief Fungsi ini dipanggil setiap kali klien mengirim paket.
 */
void OnPacketSent(Ptr<const Packet> packet)
{
    g_packetsSent++;
}

int
main(int argc, char* argv[])
{
    // --- Konfigurasi Simulasi (Disesuaikan dengan Paper) ---
    uint32_t numNodes = 40;     // Diubah dari 25 (sesuai 5.2.1, 5.2.2)
    double simTime = 200.0;     // Sesuai paper (5.2.1, 5.2.3)
    double nodeSpeed = 5.0;     // Diubah dari 0.0 (sesuai 5.2.2, 5.2.3, 5.2.4)
    double initialEnergy = 5.0; // Diubah dari 10.0 (sesuai 5.2.4)
    // UBAH BARIS INI (AGAR LEBIH PADAT)
    double arenaX = 1000.0;      // Diperkecil untuk meningkatkan kepadatan
    double arenaY = 1000.0;      // Diperkecil untuk meningkatkan kepadatan

    CommandLine cmd(__FILE__);
    cmd.AddValue("numNodes", "Jumlah node", numNodes);
    cmd.AddValue("simTime", "Waktu simulasi (detik)", simTime);
    cmd.AddValue("nodeSpeed", "Kecepatan node (m/s)", nodeSpeed);
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
    
    // BIARKAN KOSONG
    // ns-3 secara otomatis akan menggunakan default yang stabil:
    // Standar: 802.11b
    // Manajer Rate: ArfWifiManager

    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // --- Mobilitas (Mode Bergerak Sesuai Paper) ---
    MobilityHelper mobility;

    // 1. Buat batas area untuk posisi awal dan gerakan (sesuai Table 7)
    Ptr<RandomRectanglePositionAllocator> positionAlloc = CreateObject<RandomRectanglePositionAllocator>();

    // Buat variabel acak untuk X (Cara baru untuk ns-3 >= 3.40)
    Ptr<UniformRandomVariable> xRandom = CreateObject<UniformRandomVariable>();
    xRandom->SetAttribute("Min", DoubleValue(0.0));
    xRandom->SetAttribute("Max", DoubleValue(arenaX));
    positionAlloc->SetX(xRandom); // Berikan Ptr ke objek, bukan StringValue

    // Buat variabel acak untuk Y (Cara baru untuk ns-3 >= 3.40)
    Ptr<UniformRandomVariable> yRandom = CreateObject<UniformRandomVariable>();
    yRandom->SetAttribute("Min", DoubleValue(0.0));
    yRandom->SetAttribute("Max", DoubleValue(arenaY));
    positionAlloc->SetY(yRandom); // Berikan Ptr ke objek, bukan StringValue
    
    mobility.SetPositionAllocator(positionAlloc);

    // 2. Atur model mobilitas (Random Waypoint)
    // Kecepatan diatur oleh variabel nodeSpeed
    // Waktu berhenti (Pause) 1 detik
    std::string speedString = "ns3::ConstantRandomVariable[Constant=" + std::to_string(nodeSpeed) + "]";
    std::string pauseString = "ns3::ConstantRandomVariable[Constant=1.0]";

    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                              "Speed", StringValue(speedString),
                              "Pause", StringValue(pauseString),
                              "PositionAllocator", PointerValue(positionAlloc)); // Gunakan allocator yang sama untuk batas gerakan

    // 3. Install mobilitas
    mobility.Install(nodes);

    // --- Stack Internet (AODV) ---
    AodvHelper aodv; 
    aodv.Set("EnableHello", BooleanValue(false)); // <-- TAMBAHKAN BARIS INI
    // --- TAMBAHKAN BARIS INI ---
    // Memaksa HANYA node tujuan untuk membalas RREQ.
    // Ini penting agar logika EOCW Anda di node tujuan bisa berjalan.
    aodv.Set("DestinationOnly", BooleanValue(true));
    InternetStackHelper stack;
    stack.SetRoutingHelper(aodv); 
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // --- Aplikasi (UDP Echo) ---
    // Server di node 0
    UdpEchoServerHelper echoServer(9); 
    ApplicationContainer serverApps = echoServer.Install(nodes.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simTime));
    // --- MENGHUBUNGKAN TRACE SERVER ---
    serverApps.Get(0)->TraceConnectWithoutContext("Rx", MakeCallback(&OnPacketReceived));

    // Klien di node terakhir (numNodes - 1)
    UdpEchoClientHelper echoClient(interfaces.GetAddress(0), 9); 
    echoClient.SetAttribute("MaxPackets", UintegerValue(50));
    echoClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
    echoClient.SetAttribute("PacketSize", UintegerValue(1024));

    ApplicationContainer clientApps = echoClient.Install(nodes.Get(numNodes - 1));
    clientApps.Start(Seconds(2.01));
    clientApps.Stop(Seconds(simTime));
    // --- MENGHUBUNGKAN TRACE KLIEN ---
    clientApps.Get(0)->TraceConnectWithoutContext("Tx", MakeCallback(&OnPacketSent));

    // --- Logging & Animasi ---
    // --- Logging (Aktifkan log AODV level INFO) ---
    // Ini akan menyembunyikan log DEBUG/FUNCTION yang berisik
    // tapi akan MENUNJUKKAN log INFO EOCW kita.
    LogComponentEnable("AodvRoutingProtocol", LOG_LEVEL_INFO);

    AnimationInterface anim("aodv-eocw-animation.xml");
    anim.SetMobilityPollInterval(Seconds(1)); // Update posisi di NetAnim setiap 1s
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
    std::cout << " ----------------------------------" << std::endl;
    // --- AKHIR STATISTIK ---

    return 0;
}