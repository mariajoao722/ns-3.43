/*
 * Copyright (c) 2008,2009 IITP RAS
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Kirill Andreev <andreev@iitp.ru>
 *
 *
 * By default this script creates m_xSize * m_ySize square grid topology with
 * IEEE802.11s stack installed at each node with peering management
 * and HWMP protocol.
 * The side of the square cell is defined by m_step parameter.
 * When topology is created, UDP ping is installed to opposite corners
 * by diagonals. packet size of the UDP ping and interval between two
 * successive packets is configurable.
 *
 *  m_xSize * step
 *  |<--------->|
 *   step
 *  |<--->|
 *  * --- * --- * <---Ping sink  _
 *  | \   |   / |                ^
 *  |   \ | /   |                |
 *  * --- * --- * m_ySize * step |
 *  |   / | \   |                |
 *  | /   |   \ |                |
 *  * --- * --- *                _
 *  ^ Ping source
 *
 * By varying m_xSize and m_ySize, one can configure the route that is used.
 * When the inter-nodal distance is small, the source can reach the sink
 * directly.  When the inter-nodal distance is intermediate, the route
 * selected is diagonal (two hop).  When the inter-nodal distance is a bit
 * larger, the diagonals cannot be used and a four-hop route is selected.
 * When the distance is a bit larger, the packets will fail to reach even the
 * adjacent nodes.
 *
 * As of ns-3.36 release, with default configuration (mesh uses Wi-Fi 802.11a
 * standard and the ArfWifiManager rate control by default), the maximum
 * range is roughly 50m.  The default step size in this program is set to 50m,
 * so any mesh packets in the above diagram depiction will not be received
 * successfully on the diagonal hops between two nodes but only on the
 * horizontal and vertical hops.  If the step size is reduced to 35m, then
 * the shortest path will be on the diagonal hops.  If the step size is reduced
 * to 17m or less, then the source will be able to reach the sink directly
 * without any mesh hops (for the default 3x3 mesh depicted above).
 *
 * The position allocator will lay out the nodes in the following order
 * (corresponding to Node ID and to the diagram above):
 *
 * 6 - 7 - 8
 * |   |   |
 * 3 - 4 - 5
 * |   |   |
 * 0 - 1 - 2
 *
 *  See also MeshTest::Configure to read more about configurable
 *  parameters.
 */

#include "probe-metric.h"

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h" // you likely already have this
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h" // <-- needed for Ipv4FlowClassifier definition
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/ipv4-global-routing.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-list-routing.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4-static-routing.h"
#include "ns3/ipv4.h"
#include "ns3/log.h"
#include "ns3/mesh-helper.h"
#include "ns3/mesh-module.h"
#include "ns3/mesh-point-device.h"
#include "ns3/mesh-wifi-interface-mac.h"
#include "ns3/mobility-module.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"
#include "ns3/packet.h"
#include "ns3/peer-link-frame.h"
#include "ns3/peer-link.h"
#include "ns3/peer-management-protocol.h"
#include "ns3/seq-ts-header.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-phy-state.h" // WifiPhyState p/ trace "State"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-utils.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/yans-wifi-phy.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("MeshExample");

// Declaring these variables outside of main() for use in trace sinks
uint32_t g_TxCount = 0; //!< Tx packet counter.
uint32_t g_RxCount = 0;
std::map<uint32_t, std::set<uint32_t>> g_seenSeqPerNode; //!< Seen sequence numbers per node.
std::map<uint32_t, uint32_t> g_RxCountPerNode;           //!< Rx packet counter per node.
Ipv4Address multicastGroup = Ipv4Address("225.1.2.5");   // Multicast group address
uint16_t multicastPort = 8080;                           // Multicast port
// List of nodes in the multicast group
std::set<uint32_t> multicastGroupNodes = {3, 5, 6, 7, 8, 10, 11, 13, 15, 17, 19, 20, 25, 27, 29};

uint32_t g_seq = 0;

struct DelayStats
{
    uint64_t rx = 0;
    double sum = 0.0;
};

std::map<uint32_t, DelayStats> g_delayPerNode;

void
SendMulticastPacket(Ptr<Socket> socket,
                    uint32_t packetSize,
                    Ipv4Address multicastGroup,
                    InetSocketAddress remote)
{
    Ptr<Node> node = socket->GetNode();
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    Ipv4Address sourceAddress = ipv4->GetAddress(1, 0).GetLocal();

    Ptr<Packet> packet = Create<Packet>(packetSize);

    SeqTsHeader seqTs; // carries seq + timestamp
    seqTs.SetSeq(g_seq++);
    packet->AddHeader(seqTs);

    std::cout << "Sending multicast packet from " << sourceAddress << " at "
              << Simulator::Now().GetSeconds() << "s" << std::endl;

    uint32_t bytesSent = socket->SendTo(packet, 0, remote);

    if (bytesSent == packetSize + seqTs.GetSerializedSize())
    {
        std::cout << "Packet sent successfully" << std::endl;
    }
    else
    {
        std::cout << "Packet send failed. Sent " << bytesSent << " out of " << packetSize
                  << " bytes" << std::endl;
    }
    g_TxCount++;
}

void
ReceivePacket(Ptr<Socket> socket)
{
    Ptr<Node> node = socket->GetNode(); // Get the receiving node
    uint32_t nodeId = node->GetId();    // Get the node ID
    Ptr<Packet> packet;
    Address from;

    // Retrieve the packet and the sender's address
    while ((packet = socket->RecvFrom(from)))
    {
        SeqTsHeader seqTs;
        if (packet->GetSize() < seqTs.GetSerializedSize())
        {
            continue; // Packet too small to contain SeqTsHeader
        }
        packet->RemoveHeader(seqTs); // exposes the original payload
        uint32_t seq = seqTs.GetSeq();
        bool firstTimeHere = g_seenSeqPerNode[nodeId].insert(seq).second;

        if (!firstTimeHere)
        {
            std::cout << "Node " << nodeId << " already saw seq " << seq << std::endl;
            continue; // Duplicate packet
        }

        Time tx = seqTs.GetTs();    // send time stamped at sender
        Time rx = Simulator::Now(); // receive time (this node)
        double delaySec = (rx - tx).GetSeconds();

        auto& st = g_delayPerNode[nodeId];
        st.rx++;
        st.sum += delaySec;

        /*         std::cout << "Seq " << seqTs.GetSeq() << " delay to node " << nodeId << " = "
                          << delaySec * 1000.0 << " ms\n"; */
        // Check if the sender address is an IPv4 address
        InetSocketAddress senderAddress = InetSocketAddress::ConvertFrom(from);
        Ipv4Address senderIp = senderAddress.GetIpv4();

        // Get the receiver node's IP address
        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        Ipv4Address receiverIp = ipv4->GetAddress(1, 0).GetLocal();

        // Print packet information
    /*     std::cout << "Received packet of size " << packet->GetSize() << " from " << senderIp
                  << " to " << receiverIp << " at " << Simulator::Now().GetSeconds() << "s"
                  << std::endl;
 */
        g_RxCountPerNode[nodeId]++;
    }
}

/**
 * \ingroup mesh
 * \brief MeshTest class
 */
class MeshTest
{
  public:
    /// Init test
    MeshTest();
    /**
     * Configure test from command line arguments
     *
     * \param argc command line argument count
     * \param argv command line arguments
     */
    void Configure(int argc, char** argv);
    /**
     * Run test
     * \returns the test status
     */
    int Run();

  private:
    int m_xSize;             ///< X size
    int m_ySize;             ///< Y size
    double m_step;           ///< step
    double m_randomStart;    ///< random start
    double m_totalTime;      ///< total time
    double m_packetInterval; ///< packet interval
    uint16_t m_packetSize;   ///< packet size
    uint32_t m_nIfaces;      ///< number interfaces
    bool m_chan;             ///< channel
    bool m_pcap;             ///< PCAP
    bool m_ascii;            ///< ASCII
    std::string m_stack;     ///< stack
    std::string m_root;      ///< root
    /// List of network nodes
    NodeContainer nodes;
    /// List of all mesh point devices
    NetDeviceContainer meshDevices;
    /// Addresses of interfaces:
    Ipv4InterfaceContainer interfaces;
    /// MeshHelper. Report is not static methods
    MeshHelper mesh;

  private:
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor;
    /// Create nodes and setup their mobility
    void CreateNodes();
    /// Install internet m_stack on nodes
    void InstallInternetStack();
    /// Install applications
    void InstallApplication();
    /// Install unicast UDP ping application from src to dst
    void InstallUnicastTraffic(uint32_t src,
                               uint32_t dst,
                               uint16_t port,
                               uint32_t packetSize,
                               std::string rate,
                               double start,
                               double stop);
    /// Print mesh devices diagnostics
    void Report();
};

MeshTest::MeshTest()
    : m_xSize(4),
      m_ySize(1),
      m_step(40.0),
      m_randomStart(0.1),
      m_totalTime(80.0),
      m_packetInterval(1),
      m_packetSize(1024),
      m_nIfaces(1),
      m_chan(true),
      m_pcap(true),
      m_ascii(true),
      m_stack("ns3::Dot11sStack"),
      m_root("ff:ff:ff:ff:ff:ff")
{
}

void
MeshTest::Configure(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.AddValue("x-size", "Number of nodes in a row grid", m_xSize);
    cmd.AddValue("y-size", "Number of rows in a grid", m_ySize);
    cmd.AddValue("step", "Size of edge in our grid (meters)", m_step);
    // Avoid starting all mesh nodes at the same time (beacons may collide)
    cmd.AddValue("start", "Maximum random start delay for beacon jitter (sec)", m_randomStart);
    cmd.AddValue("time", "Simulation time (sec)", m_totalTime);
    cmd.AddValue("packet-interval", "Interval between packets in UDP ping (sec)", m_packetInterval);
    cmd.AddValue("packet-size", "Size of packets in UDP ping (bytes)", m_packetSize);
    cmd.AddValue("interfaces", "Number of radio interfaces used by each mesh point", m_nIfaces);
    cmd.AddValue("channels", "Use different frequency channels for different interfaces", m_chan);
    cmd.AddValue("pcap", "Enable PCAP traces on interfaces", m_pcap);
    cmd.AddValue("ascii", "Enable Ascii traces on interfaces", m_ascii);
    cmd.AddValue("stack", "Type of protocol stack. ns3::Dot11sStack by default", m_stack);
    cmd.AddValue("root", "Mac address of root mesh point in HWMP", m_root);

    cmd.Parse(argc, argv);
    NS_LOG_DEBUG("Grid:" << m_xSize << "*" << m_ySize);
    NS_LOG_DEBUG("Simulation time: " << m_totalTime << " s");
    if (m_ascii)
    {
        PacketMetadata::Enable();
    }
}

void
MeshTest::CreateNodes()
{
    nodes.Create(30);
    std::cout << "Number of nodes created: " << nodes.GetN() << std::endl;

    // Configure YansWifiChannel
    YansWifiPhyHelper wifiPhy;
    YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default();
    wifiPhy.SetChannel(wifiChannel.Create());
    /*
     * Create mesh helper and set stack installer to it
     * Stack installer creates all needed protocols and install them to
     * mesh point device
     */
    mesh = MeshHelper::Default();
    if (!Mac48Address(m_root.c_str()).IsBroadcast())
    {
        mesh.SetStackInstaller(m_stack, "Root", Mac48AddressValue(Mac48Address(m_root.c_str())));
    }
    else
    {
        // If root is not set, we do not use "Root" attribute, because it
        // is specified only for 11s
        mesh.SetStackInstaller(m_stack);
    }
    if (m_chan)
    {
        mesh.SetSpreadInterfaceChannels(MeshHelper::SPREAD_CHANNELS);
    }
    else
    {
        mesh.SetSpreadInterfaceChannels(MeshHelper::ZERO_CHANNEL);
    }
    mesh.SetMacType("RandomStart", TimeValue(Seconds(m_randomStart)));
    // Set number of interfaces - default is single-interface mesh point
    mesh.SetNumberOfInterfaces(m_nIfaces);

    // Install protocols and return container if MeshPointDevices
    meshDevices = mesh.Install(wifiPhy, nodes);
    // AssignStreams can optionally be used to control random variable streams
    mesh.AssignStreams(meshDevices, 0);
    // Setup mobility - static grid topology
    MobilityHelper mobility;

    // Define o intervalo em X e Y (por ex., 0 a 100 metros em cada eixo)
    Ptr<UniformRandomVariable> randomX = CreateObject<UniformRandomVariable>();
    randomX->SetAttribute("Min", DoubleValue(0.0));
    randomX->SetAttribute("Max", DoubleValue(300.0));

    Ptr<UniformRandomVariable> randomY = CreateObject<UniformRandomVariable>();
    randomY->SetAttribute("Min", DoubleValue(0.0));
    randomY->SetAttribute("Max", DoubleValue(300.0));

    Ptr<RandomRectanglePositionAllocator> positionAlloc =
        CreateObject<RandomRectanglePositionAllocator>();
    positionAlloc->SetX(randomX);
    positionAlloc->SetY(randomY);

    mobility.SetPositionAllocator(positionAlloc);

    mobility.Install(nodes);

    if (m_pcap)
    {
        wifiPhy.EnablePcapAll(
            std::string("/home/mpais/ns-allinone-3.43/ns-3.43/scratch/meshtrace3/mp"));
    }
    if (m_ascii)
    {
        AsciiTraceHelper ascii;
        wifiPhy.EnableAsciiAll(ascii.CreateFileStream("mesh.tr"));
    }

    // Now back‐patch the device pointer into your HWMP object:
    for (NetDeviceContainer::Iterator it = meshDevices.Begin(); it != meshDevices.End(); ++it)
    {
        Ptr<NetDevice> dev = *it;

        Ptr<MeshPointDevice> mpd = dev->GetObject<MeshPointDevice>();
        Ptr<ns3::dot11s::HwmpProtocol> hwmp = mpd->GetObject<ns3::dot11s::HwmpProtocol>();
        hwmp->SetDevice(mpd);

        hwmp->StartLinkMonitor(Seconds(7));
    }

    HookPhyOccupancyAllNodes();
}

void
MeshTest::InstallInternetStack()
{
    InternetStackHelper internetStack;

    internetStack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    interfaces = address.Assign(meshDevices);

    Ipv4StaticRoutingHelper multicastRoutingHelper;

    Ptr<Node> sender = nodes.Get(0);
    Ptr<NetDevice> senderIf = meshDevices.Get(0);
    multicastRoutingHelper.SetDefaultMulticastRoute(sender, senderIf);
}

void
MeshTest::InstallApplication()
{
    // Set up a multicast receiver on the node
    for (uint32_t i = 1; i < nodes.GetN(); ++i)
    {
        // Check if the node is in the multicast group
        if (multicastGroupNodes.find(i) != multicastGroupNodes.end())
        {
            Ptr<Socket> recvSink =
                Socket::CreateSocket(nodes.Get(i), UdpSocketFactory::GetTypeId());
            // Define the IP address and port for the socket to listen on
            InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), multicastPort);

            // Allow the socket to receive broadcast packets
            recvSink->SetAllowBroadcast(true);
            // Set the callback function to be called whenever the socket receives a packet
            recvSink->SetRecvCallback(MakeCallback(&ReceivePacket));

            // Bind the socket to the configured address and port
            recvSink->Bind(local);

            std::cout << "Setting up multicast receiver on node " << i << std::endl;

            Ptr<MeshPointDevice> mpd = nodes.Get(i)->GetDevice(0)->GetObject<MeshPointDevice>();
            Ptr<dot11s::HwmpProtocol> hwmp = mpd->GetObject<dot11s::HwmpProtocol>();
            hwmp->SetMulticastGroupNodes(
                Mac48Address::ConvertFrom(nodes.Get(i)->GetDevice(0)->GetAddress()));
        }
        else
        {
            std::cout << "Node " << i << " is not in the multicast group. Skipping receiver setup."
                      << std::endl;
        }
    }

    // Set up a multicast sender on the first node
    // Obtém o objeto Ipv4 do nó 0
    Ptr<Ipv4> ipv4 = nodes.Get(0)->GetObject<Ipv4>();

    // Obtém o endereço IP da interface 1 do nó 0
    Ipv4Address ipAddress = ipv4->GetAddress(1, 0).GetLocal();

    Ptr<Socket> source = Socket::CreateSocket(nodes.Get(0), UdpSocketFactory::GetTypeId());
    // Liga o socket ao endereço IP "10.1.1.1" e à porta 8080 no emissor.
    source->Bind(InetSocketAddress(ipAddress, multicastPort));
    // Define o endereço de destino do multicast (grupo multicast e porta)
    InetSocketAddress remote = InetSocketAddress(multicastGroup, multicastPort);
    // Permite que o socket envie pacotes em broadcast
    source->SetAllowBroadcast(true);

    //  Estabelece a conexão com o grupo multicast e a porta
    source->Connect(remote);

    double startTime = 10.0;
    double endTime = m_totalTime;
    double interval = 1.0 / 90.0;
    for (double t = startTime; t <= endTime; t += interval)
    {
        Simulator::Schedule(Seconds(t), // Send every second from 10s
                            &SendMulticastPacket,
                            source,
                            m_packetSize,
                            multicastGroup,
                            remote);
    }
}

void
MeshTest::InstallUnicastTraffic(uint32_t src,
                                uint32_t dst,
                                uint16_t port,
                                uint32_t packetSize,
                                std::string rate,
                                double start,
                                double stop)
{
    Ipv4Address dstIp = interfaces.GetAddress(dst);

    // Create UDP sink on dst
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(nodes.Get(dst));
    sinkApp.Start(Seconds(start - 0.5)); // start a bit before sender
    sinkApp.Stop(Seconds(stop + 0.5));

    // Create OnOff source on src
    OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(dstIp, port));
    onoff.SetAttribute("PacketSize", UintegerValue(packetSize));
    onoff.SetAttribute("DataRate", StringValue(rate));
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer srcApp = onoff.Install(nodes.Get(src));
    srcApp.Start(Seconds(start));
    srcApp.Stop(Seconds(stop));
}

int
MeshTest::Run()
{
    //LogComponentEnable("HwmpProtocol", LOG_LEVEL_ALL);
    //LogComponentEnable("HwmpProtocolMac", LOG_LEVEL_ALL);

    CreateNodes();
    InstallInternetStack();
    InstallApplication();
    //InstallUnicastTraffic(/*src=*/0,
      //                    /*dst=*/6,
       //                   /*port=*/9001,
        //                  /*packetSize=*/512,
        //                  /*rate=*/"5Mbps",
          //                /*start=*/12.0,
         //                 /*stop=*/m_totalTime - 2.0);

    AnimationInterface anim("mesh32.xml");

    Simulator::Schedule(Seconds(m_totalTime), &MeshTest::Report, this);
    Simulator::Stop(Seconds(m_totalTime + 2));

    monitor = flowmon.InstallAll();

    Simulator::Run();

    std::cout << "UDP echo packets sended: " << g_TxCount << std::endl;

    for (const auto& entry : g_RxCountPerNode)
    {
        uint32_t nodeId = entry.first;
        uint32_t rxCount = entry.second;
        double deliveryRatio =
            (g_TxCount > 0) ? (static_cast<double>(rxCount) / g_TxCount) * 100.0 : 0.0;
        std::cout << "Node " << nodeId << " received: " << rxCount << " packets"
                  << " (Delivery ratio: " << deliveryRatio << "%)" << std::endl;
    }

    // === PDR global ===
    // sumPr = soma dos |P_r| (pacotes únicos recebidos por todos os receptores)
    uint64_t sumPr = 0;
    for (const auto& kv : g_RxCountPerNode)
        sumPr += kv.second;

    // R = nº de receptores pretendidos
    uint32_t R = multicastGroupNodes.size();

    double pdr_global = (R && g_TxCount) ? (double)sumPr / (R * (double)g_TxCount) * 100.0 : 0.0;

    // === AvgEED global ===
    // média dos (Rt_p - St_p) apenas sobre pacotes únicos

    double sumDelay = 0.0;
    uint64_t totalRecv = 0;
    for (auto& kv : g_delayPerNode)
    {
        sumDelay += kv.second.sum;
        totalRecv += kv.second.rx;
    }
    double avgEED_ms = totalRecv ? (sumDelay / totalRecv) * 1000.0 : 0.0;

    std::cout << "-----------------------------\n";
    std::cout << "Global PDR : " << pdr_global << " %\n";
    std::cout << "Global Avg E2E delay: " << avgEED_ms << " ms\n";
    std::cout << "-----------------------------\n";

    // === AvgEED node ===
    for (const auto& kv : g_delayPerNode)
    {
        uint32_t nodeId = kv.first;
        const auto& st = kv.second;
        if (st.rx == 0)
            continue;

        double mean = st.sum / st.rx; // average delay in seconds

        std::cout << "============0============\n"
                  << "Node " << nodeId << " delay stats:\n"
                  << "avg=" << mean * 1000.0 << " ms\n"
                  << "============0============\n"
                  << std::endl;
    }

    PrintPhyOccupancySummary();
/* 
    monitor->CheckForLostPackets();
    monitor->SerializeToXmlFile("mesh-flows.xml", true, true);
    std::cout << "Unicast flow monitor statistics:\n";

    // (Optional) quick print per-flow throughput
    double simTime = m_totalTime; // seconds
    for (auto& kv : monitor->GetFlowStats())
    {
        auto id = kv.first;
        auto st = kv.second;
        double txMbit = (st.txBytes * 8.0) / 1e6;
        double rxMbit = (st.rxBytes * 8.0) / 1e6;
        std::cout << "[Flow " << id << "] TX=" << txMbit / simTime << " Mb/s "
                  << " RX=" << rxMbit / simTime << " Mb/s "
                  << " Lost=" << st.lostPackets << " DelayAvg="
                  << (st.delaySum.GetSeconds() / std::max<uint64_t>(1, st.rxPackets)) * 1000.0
                  << " ms\n";
    }
    std::cout << "Output flow monitor statistics:\n";
    auto classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    auto stats = monitor->GetFlowStats();
    for (const auto& kv : stats)
    {
        const auto& st = kv.second;
        std::cout << "[Flow " << kv.first << "] "
                  << "txPackets=" << st.txPackets << " rxPackets=" << st.rxPackets
                  << " lostPackets=" << st.lostPackets << " txBytes=" << st.txBytes
                  << " rxBytes=" << st.rxBytes << std::endl;
    } */

    Simulator::Destroy();

    return 0;
}

void
MeshTest::Report()
{
    std::ostringstream out;
    out << "reports/mesh-report344.txt";
    std::cerr << "Printing overall mesh diagnostics to " << out.str() << "\n";
    std::ofstream of;
    of.open(out.str().c_str(), std::ios::app);
    if (!of.is_open())
    {
        std::cerr << "Error: Can't open file " << out.str() << "\n";
        return;
    }

    of << "Number of nodes: " << nodes.GetN() << "\n";
    of << "Grid: " << m_xSize << "*" << m_ySize << "\n";
    of << "Simulation time: " << m_totalTime << " s\n";
    of << "------------------------------\n";

    of << "\n================ NEW RUN ================\n";
    of << "Simulation started at t=" << Simulator::Now().GetSeconds() << "s\n";

    // --- cabeçalho: enviados + recebidos por nó
    of << "UDP echo packets sended: " << g_TxCount << "\n";
    for (const auto& entry : g_RxCountPerNode)
    {
        uint32_t nodeId = entry.first;
        uint32_t rxCount = entry.second;
        double deliveryRatio =
            (g_TxCount > 0) ? (static_cast<double>(rxCount) / g_TxCount) * 100.0 : 0.0;
        of << "Node " << nodeId << " received: " << rxCount << " packets"
           << " (Delivery ratio: " << deliveryRatio << "%)" << std::endl;
    }
    of << "-----------------------------\n";

    // === PDR global ===
    // sumPr = soma dos |P_r| (pacotes únicos recebidos por todos os receptores)
    uint64_t sumPr = 0;
    for (const auto& kv : g_RxCountPerNode)
        sumPr += kv.second;

    // R = nº de receptores pretendidos
    uint32_t R = multicastGroupNodes.size();

    double pdr_global = (R && g_TxCount) ? (double)sumPr / (R * (double)g_TxCount) * 100.0 : 0.0;

    // === AvgEED global ===
    // média dos (Rt_p - St_p) apenas sobre pacotes únicos

    double sumDelay = 0.0;
    uint64_t totalRecv = 0;
    for (auto& kv : g_delayPerNode)
    {
        sumDelay += kv.second.sum;
        totalRecv += kv.second.rx;
    }
    double avgEED_ms = totalRecv ? (sumDelay / totalRecv) * 1000.0 : 0.0;

    of << "-----------------------------\n";
    of << "Global PDR : " << pdr_global << " %\n";
    of << "Global Avg E2E delay: " << avgEED_ms << " ms\n";
    of << "-----------------------------\n";

    // === AvgEED node ===
    for (const auto& kv : g_delayPerNode)
    {
        uint32_t nodeId = kv.first;
        const auto& st = kv.second;
        if (st.rx == 0)
            continue;

        double mean = st.sum / st.rx; // average delay in seconds

        of << "============0============\n"
           << "Node " << nodeId << " delay stats:\n"
           << "avg=" << mean * 1000.0 << " ms\n"
           << "============0============\n"
           << std::endl;
    }

    PrintPhyOccupancySummary(of);
/* 
    of << "==============================\n";
    of << "Unicast flow monitor statistics:\n";
    of << "==============================\n";

    auto classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    auto stats = monitor->GetFlowStats();
    for (const auto& kv : stats)
    {
        const auto& st = kv.second;
        of << "[Flow " << kv.first << "] "
           << "txPackets=" << st.txPackets << " rxPackets=" << st.rxPackets
           << " lostPackets=" << st.lostPackets << " txBytes=" << st.txBytes
           << " rxBytes=" << st.rxBytes << "\n";
    } */

    of.close();
}

int
main(int argc, char* argv[])
{
    // Enable packet metadata at the very start
    PacketMetadata::Enable();
        uint32_t rngRun = 1;
    CommandLine cmd(__FILE__);

    cmd.AddValue("RngRun", "Global RNG run", rngRun);
    cmd.Parse(argc, argv);
    ns3::RngSeedManager::SetSeed(12345); // opcional — default é 1
    ns3::RngSeedManager::SetRun(rngRun);
    MeshTest t;
    t.Configure(argc, argv);
    return t.Run();
}
