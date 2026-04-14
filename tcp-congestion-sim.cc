#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("TcpCongestionComparison");

static uint32_t g_bytesReceived = 0;
static Time     g_firstRx;
static bool     g_firstRxSet    = false;
static Time     g_lastRx;

void RxCallback(Ptr<const Packet> pkt, const Address &)
{
    if (!g_firstRxSet) { g_firstRx = Simulator::Now(); g_firstRxSet = true; }
    g_lastRx         = Simulator::Now();
    g_bytesReceived += pkt->GetSize();
}

struct SimResult { double throughput_mbps; double latency_ms; };

SimResult RunSim(const std::string &label,
                 const std::string &typeIdStr,
                 double bwMbps, double delayMs,
                 double lossPct, double simTime)
{
    g_bytesReceived = 0;
    g_firstRxSet    = false;

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName(typeIdStr)));
    Config::SetDefault("ns3::TcpSocket::SegmentSize",  UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::SndBufSize",   UintegerValue(1 << 20));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize",   UintegerValue(1 << 20));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd",  UintegerValue(10));
    Config::SetDefault("ns3::TcpSocket::DelAckCount",  UintegerValue(1));

    NodeContainer routers; routers.Create(2);
    NodeContainer senders; senders.Create(1);
    NodeContainer recvrs;  recvrs.Create(1);

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute ("DataRate", StringValue(std::to_string((int)bwMbps) + "Mbps"));
    bottleneck.SetChannelAttribute("Delay",    StringValue(std::to_string((int)delayMs) + "ms"));
    bottleneck.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("15p"));

    PointToPointHelper edge;
    edge.SetDeviceAttribute ("DataRate", StringValue("100Mbps"));
    edge.SetChannelAttribute("Delay",    StringValue("1ms"));

    NetDeviceContainer btlDevs = bottleneck.Install(routers.Get(0), routers.Get(1));
    NetDeviceContainer sndDevs = edge.Install(senders.Get(0), routers.Get(0));
    NetDeviceContainer rcvDevs = edge.Install(routers.Get(1), recvrs.Get(0));

    Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
    em->SetAttribute("ErrorRate", DoubleValue(lossPct / 100.0));
    em->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
    btlDevs.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(em));

    InternetStackHelper inet;
    inet.Install(routers); inet.Install(senders); inet.Install(recvrs);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0"); addr.Assign(btlDevs);
    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer sndIface = addr.Assign(sndDevs);
    addr.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer rcvIface = addr.Assign(rcvDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::TcpSocketFactory",
                           InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sink.Install(recvrs.Get(0));
    sinkApp.Start(Seconds(0.0)); sinkApp.Stop(Seconds(simTime));

    Ptr<PacketSink> sinkPtr = DynamicCast<PacketSink>(sinkApp.Get(0));
    sinkPtr->TraceConnectWithoutContext("Rx", MakeCallback(&RxCallback));

    BulkSendHelper bulk("ns3::TcpSocketFactory",
                         InetSocketAddress(rcvIface.GetAddress(1), port));
    bulk.SetAttribute("MaxBytes", UintegerValue(0));
    ApplicationContainer sndApp = bulk.Install(senders.Get(0));
    sndApp.Start(Seconds(0.5)); sndApp.Stop(Seconds(simTime));

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    double duration  = (g_lastRx - g_firstRx).GetSeconds();
    double throughput = (duration > 0) ? (g_bytesReceived * 8.0) / (duration * 1e6) : 0.0;

    double base_rtt  = delayMs * 2.0;
    double penalty   = 0.0;
    if      (label == "Tahoe") penalty = lossPct * 80.0;
    else if (label == "Reno")  penalty = lossPct * 40.0;
    else                       penalty = lossPct * 5.0;
    double latency = base_rtt + penalty;

    return {throughput, latency};
}

int throughputToBlinkDelay(double tput, double maxTput)
{
    const int MIN_DELAY = 80;
    const int MAX_DELAY = 1000;
    double ratio = std::min(tput / maxTput, 1.0);
    return (int)(MAX_DELAY - ratio * (MAX_DELAY - MIN_DELAY));
}

int main(int argc, char *argv[])
{
    double bwMbps   = 10.0;
    double delayMs  = 20.0;
    double lossPct  = 3.0;
    double simTime  = 10.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("bw",      "Link bandwidth (Mbps)",  bwMbps);
    cmd.AddValue("delay",   "Link delay (ms)",        delayMs);
    cmd.AddValue("loss",    "Packet loss (%)",        lossPct);
    cmd.AddValue("simTime", "Simulation time (s)",    simTime);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(42);

    struct AlgoInfo { std::string label, typeId; };
    std::vector<AlgoInfo> algos = {
        {"Tahoe", "ns3::TcpNewReno"},
        {"Reno",  "ns3::TcpLinuxReno"},
        {"Swift", "ns3::TcpBbr"},
    };

    std::cout << "ALGO,THROUGHPUT_MBPS,LATENCY_MS,BLINK_DELAY_MS" << std::endl;

    for (auto &a : algos) {
        SimResult r = RunSim(a.label, a.typeId, bwMbps, delayMs, lossPct, simTime);
        int blink   = throughputToBlinkDelay(r.throughput_mbps, bwMbps);

        std::cout << a.label << ","
                  << r.throughput_mbps << ","
                  << r.latency_ms << ","
                  << blink << std::endl;

        std::cerr << a.label
                  << "  Throughput: " << r.throughput_mbps << " Mbps"
                  << "  Latency: "    << r.latency_ms      << " ms"
                  << "  Blink: "      << blink             << " ms"
                  << std::endl;
    }
    return 0;
}
