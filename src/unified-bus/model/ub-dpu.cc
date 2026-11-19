// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-dpu.h"

#include "ns3/ub-datatype.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/enum.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbDpu);
NS_LOG_COMPONENT_DEFINE("UbDpu");

UbDpu::UbDpu() = default;

UbDpu::~UbDpu() = default;

TypeId
UbDpu::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbDpu")
            .SetParent<UbSwitch>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbDpu>()
            .AddAttribute("UbFlowControl",
                          "Flow-control mechanism for UB-side ports.",
                          EnumValue(FcType::CBFC),
                          MakeEnumAccessor<FcType>(&UbDpu::m_ubFlowControlType),
                          MakeEnumChecker(FcType::NONE, "NONE",
                                          FcType::CBFC, "CBFC",
                                          FcType::CBFC_SHARED, "CBFC_SHARED"))
            .AddAttribute("RoceFlowControl",
                          "Flow-control mechanism for RoCE-side ports.",
                          EnumValue(FcType::PFC_DYNAMIC_PAPER),
                          MakeEnumAccessor<FcType>(&UbDpu::m_roceFlowControlType),
                          MakeEnumChecker(FcType::NONE, "NONE",
                                          FcType::PFC_FIXED, "PFC_FIXED",
                                          FcType::PFC_DYNAMIC, "PFC_DYNAMIC",
                                          FcType::PFC_DYNAMIC_PAPER, "PFC_DYNAMIC_PAPER"))
            .AddAttribute("CcRole",
                          "Congestion-control role of the DPU: TRANSPARENT (pure forwarder) or "
                          "RP (RoCE reaction point that rate-limits data toward the RoCE side).",
                          EnumValue(CcRole::TRANSPARENT),
                          MakeEnumAccessor<CcRole>(&UbDpu::m_ccRole),
                          MakeEnumChecker(CcRole::TRANSPARENT, "TRANSPARENT",
                                          CcRole::RP, "RP"))
            .AddAttribute("RpAllocAlgo",
                          "Bandwidth allocation algorithm for RP mode across flows to the same "
                          "destination NPU: PROPORTIONAL, MAXMIN, or WEIGHTED.",
                          EnumValue(RpAllocAlgo::PROPORTIONAL),
                          MakeEnumAccessor<RpAllocAlgo>(&UbDpu::m_rpAllocAlgo),
                          MakeEnumChecker(                                          RpAllocAlgo::PROPORTIONAL, "PROPORTIONAL",
                                          RpAllocAlgo::MAXMIN, "MAXMIN",
                                          RpAllocAlgo::WEIGHTED, "WEIGHTED"))
            .AddAttribute("RpPushEnabled",
                          "When true, the RP actively pushes its per-flow rate back to each "
                          "source ASU on every allocation update.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbDpu::m_rpPushEnabled),
                          MakeBooleanChecker())
            .AddAttribute("RpPfcFeedbackEnabled",
                          "When true, a received PFC pause immediately triggers the RP to "
                          "reallocate and push rates. Set false to rely only on CNP feedback.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&UbDpu::m_rpPfcFeedbackEnabled),
                          MakeBooleanChecker())
            .AddAttribute("RpLineRate",
                          "Line rate used to clamp RP per-flow rates when CcRole == RP.",
                          DataRateValue(DataRate("400Gbps")),
                          MakeDataRateAccessor(&UbDpu::m_rpLineRate),
                          MakeDataRateChecker())
            .AddAttribute("RpInitialRate",
                          "Initial per-flow rate when a flow is first observed in RP mode.",
                          DataRateValue(DataRate("400Gbps")),
                          MakeDataRateAccessor(&UbDpu::m_rpInitialRate),
                          MakeDataRateChecker())
            .AddAttribute("RpRateAi",
                          "Additive-increase step for a destination's available-bandwidth estimate.",
                          DataRateValue(DataRate("100Mbps")),
                          MakeDataRateAccessor(&UbDpu::m_rpRateAi),
                          MakeDataRateChecker())
            .AddAttribute("RpUpdateInterval",
                          "Interval at which the RP recomputes per-destination bandwidth "
                          "estimates and reallocates rates across flows.",
                          TimeValue(MicroSeconds(55)),
                          MakeTimeAccessor(&UbDpu::m_rpUpdateInterval),
                          MakeTimeChecker())
            .AddAttribute("RpAlphaGain",
                          "Multiplicative-decrease factor applied to a destination's bandwidth "
                          "estimate when a CNP for it is seen.",
                          DoubleValue(0.00390625),
                          MakeDoubleAccessor(&UbDpu::m_rpAlphaGain),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("RpBacklogThreshold",
                          "RoCE egress backlog in bytes above which the RP treats itself as "
                          "congested and cuts per-destination bandwidth immediately (fast "
                          "CNP-independent feedback).",
                          UintegerValue(65536),
                          MakeUintegerAccessor(&UbDpu::m_rpBacklogThreshold),
                          MakeUintegerChecker<uint64_t>());
    return tid;
}

void
UbDpu::SetPortSide(uint32_t portId, PortSide side)
{
    if (portId >= m_portSides.size())
    {
        m_portSides.resize(portId + 1, PortSide::UB);
    }
    m_portSides[portId] = side;
}

UbDpu::PortSide
UbDpu::GetPortSide(uint32_t portId) const
{
    if (portId >= m_portSides.size())
    {
        return PortSide::UB;
    }
    return m_portSides[portId];
}

bool
UbDpu::IsUbSide(uint32_t portId) const
{
    return GetPortSide(portId) == PortSide::UB;
}

bool
UbDpu::IsRoceSide(uint32_t portId) const
{
    return GetPortSide(portId) == PortSide::ROCE;
}

FcType
UbDpu::GetFlowControlTypeForPort(uint32_t portId) const
{
    return IsRoceSide(portId) ? m_roceFlowControlType : m_ubFlowControlType;
}

DataRate
UbDpu::DeriveRoceLineRate() const
{
    DataRate best{"0bps"};
    Ptr<Node> node = GetObject<Node>();
    if (node != nullptr)
    {
        for (uint32_t index = 0; index < node->GetNDevices(); ++index)
        {
            if (!IsRoceSide(index))
            {
                continue;
            }
            Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(index));
            if (port != nullptr && port->GetDataRate() > best)
            {
                best = port->GetDataRate();
            }
        }
    }
    return best.GetBitRate() == 0 ? m_rpLineRate : best;
}

DataRate
UbDpu::DeriveNpuLineRate(uint32_t destinationNodeId) const
{
    (void)destinationNodeId;
    // The DPU does not see the downstream NPU link directly; start from its RoCE line rate
    // and let CNP feedback shrink the estimate toward the real bottleneck.
    return DeriveRoceLineRate();
}

UbDpu::DpuFlowState&
UbDpu::GetOrCreateFlowState(const DpuFlowKey& key)
{
    auto it = m_flowStates.find(key);
    if (it == m_flowStates.end())
    {
        DpuFlowState state;
        state.currentRate = m_rpInitialRate;
        state.nextSendTime = Simulator::Now();
        state.senderStarted = true;
        it = m_flowStates.emplace(key, std::move(state)).first;
    }
    return it->second;
}

UbDpu::DpuDestState&
UbDpu::GetOrCreateDestState(uint32_t destinationNodeId)
{
    auto it = m_destStates.find(destinationNodeId);
    if (it == m_destStates.end())
    {
        DpuDestState state;
        state.availableBandwidth = DeriveNpuLineRate(destinationNodeId);
        it = m_destStates.emplace(destinationNodeId, std::move(state)).first;
    }
    return it->second;
}

void
UbDpu::OnCnpSeen(const DpuFlowKey& key)
{
    DpuDestState& dest = GetOrCreateDestState(key.destinationNodeId);
    const uint64_t line = DeriveNpuLineRate(key.destinationNodeId).GetBitRate();
    const double cutFactor = 1.0 - dest.alpha / 2.0;
    double current = dest.availableBandwidth.GetBitRate();
    const uint64_t next = std::max<uint64_t>(
        1u, std::min<uint64_t>(static_cast<uint64_t>(current * cutFactor), line));
    dest.availableBandwidth = DataRate(next);
    dest.alpha = (1.0 - m_rpAlphaGain) * dest.alpha + m_rpAlphaGain;
    dest.congested = true;
}

void
UbDpu::OnDataPacket(const DpuFlowKey& key, uint32_t bytes)
{
    DpuFlowState& flow = GetOrCreateFlowState(key);
    flow.windowBytes += bytes;
}

void
UbDpu::PushRateToAsu(const DpuFlowKey& key, DataRate rate)
{
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return;
    }

    Ptr<Packet> packet = Create<Packet>();

    // UbCnpExtTph is the CNP payload; it must be the inner-most header (added first).
    UbCnpExtTph ext;
    ext.SetRateMbps(static_cast<uint32_t>(rate.GetBitRate() / 1000000ULL));
    packet->AddHeader(ext);

    UbCompactEidHeader eidHeader;
    eidHeader.SetSourceEid(1);
    eidHeader.SetDestinationEid(1);
    packet->AddHeader(eidHeader);

    UbCompactUpiHeader upiHeader;
    upiHeader.SetUpi(0);
    packet->AddHeader(upiHeader);

    UbCtpHeader ctpHeader;
    ctpHeader.SetTPOpcode(CtpOpcode::CTP_CNP);
    ctpHeader.SetNlp(UB_CTPH_NLP_UPI16_EID40_TAH);
    packet->AddHeader(ctpHeader);

    UbCna16NetworkHeader cnaHeader;
    cnaHeader.SetScna(static_cast<uint16_t>(utils::NodeIdToCna16(key.destinationNodeId)));
    cnaHeader.SetDcna(static_cast<uint16_t>(utils::NodeIdToCna16(key.sourceNodeId)));
    cnaHeader.SetServiceLevel(key.vl);
    cnaHeader.SetNlp(UB_CNA_NLP_CTPH);
    packet->AddHeader(cnaHeader);

    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                key.vl,
                                key.vl,
                                RoutingType::PER_FLOW_SHORTEST_PATHS,
                                UbDatalinkHeaderConfig::PACKET_CNA16);

    for (uint32_t port = 0; port < node->GetNDevices(); ++port)
    {
        if (!IsUbSide(port))
        {
            continue;
        }
        SendPacket(packet->Copy(), port, port, key.vl);
    }
}

uint32_t
UbDpu::FlowWeight(const DpuFlowKey& key) const
{
    // Lower VL (higher priority) gets a larger weight in the WEIGHTED allocator.
    return std::max<uint32_t>(1u, 16u - std::min<uint32_t>(key.vl, 15u));
}

void
UbDpu::UpdateDemands()
{
    const Time now = Simulator::Now();
    if (m_lastDemandUpdate.IsZero())
    {
        m_lastDemandUpdate = now;
        return;
    }
    const Time elapsed = now - m_lastDemandUpdate;
    if (elapsed.IsZero())
    {
        return;
    }
    const double seconds = elapsed.GetSeconds();
    for (auto& entry : m_flowStates)
    {
        DpuFlowState& flow = entry.second;
        flow.demand = DataRate(std::max<uint64_t>(
            1u, static_cast<uint64_t>(static_cast<double>(flow.windowBytes) * 8.0 / seconds)));
        flow.windowBytes = 0;
    }
    m_lastDemandUpdate = now;
}

std::vector<DataRate>
UbDpu::Allocate(const std::vector<DpuFlowKey>& keys,
                const std::vector<DataRate>& demands,
                DataRate bandwidth) const
{
    const std::size_t n = keys.size();
    std::vector<double> d(n, 0.0);
    const double bw = static_cast<double>(bandwidth.GetBitRate());
    for (std::size_t i = 0; i < n; ++i)
    {
        d[i] = static_cast<double>(demands[i].GetBitRate());
    }
    std::vector<double> r(n, 0.0);

    switch (m_rpAllocAlgo)
    {
    case RpAllocAlgo::PROPORTIONAL: {
        double sum = 0.0;
        for (double v : d)
        {
            sum += v;
        }
        if (sum <= 0.0)
        {
            for (std::size_t i = 0; i < n; ++i)
            {
                r[i] = bw / static_cast<double>(n);
            }
            break;
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            r[i] = bw * d[i] / sum;
        }
        break;
    }
    case RpAllocAlgo::MAXMIN: {
        std::vector<bool> saturated(n, false);
        double remaining = bw;
        std::size_t unsat = n;
        for (std::size_t iter = 0; iter < n; ++iter)
        {
            if (unsat == 0)
            {
                break;
            }
            const double share = remaining / static_cast<double>(unsat);
            bool changed = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (saturated[i])
                {
                    continue;
                }
                if (d[i] <= share)
                {
                    r[i] = d[i];
                    remaining -= d[i];
                    saturated[i] = true;
                    --unsat;
                    changed = true;
                }
            }
            if (!changed)
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (!saturated[i])
                    {
                        r[i] = share;
                    }
                }
                break;
            }
        }
        break;
    }
    case RpAllocAlgo::WEIGHTED: {
        double weightedSum = 0.0;
        std::vector<double> w(n, 1.0);
        for (std::size_t i = 0; i < n; ++i)
        {
            w[i] = static_cast<double>(FlowWeight(keys[i]));
            weightedSum += w[i] * d[i];
        }
        if (weightedSum <= 0.0)
        {
            double wsum = 0.0;
            for (double v : w)
            {
                wsum += v;
            }
            for (std::size_t i = 0; i < n; ++i)
            {
                r[i] = bw * w[i] / wsum;
            }
            break;
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            r[i] = bw * (w[i] * d[i]) / weightedSum;
        }
        break;
    }
    }

    std::vector<DataRate> result(n, DataRate(0));
    for (std::size_t i = 0; i < n; ++i)
    {
        result[i] = DataRate(std::max<uint64_t>(1u, static_cast<uint64_t>(r[i])));
    }
    return result;
}

void
UbDpu::RecomputeAllocation()
{
    UpdateDemands();

    // Collect every destination that has at least one observed flow (whether congested or not),
    // so uncongested flows such as the victim still get a rate assigned.
    std::set<uint32_t> destinations;
    for (const auto& flowEntry : m_flowStates)
    {
        destinations.insert(flowEntry.first.destinationNodeId);
    }

    for (uint32_t dst : destinations)
    {
        DpuDestState& dest = GetOrCreateDestState(dst);
        const uint64_t line = DeriveNpuLineRate(dst).GetBitRate();
        if (dest.congested)
        {
            const double cutFactor = 1.0 - dest.alpha / 2.0;
            const uint64_t next = std::max<uint64_t>(
                1u, std::min<uint64_t>(static_cast<uint64_t>(dest.availableBandwidth.GetBitRate() *
                                                            cutFactor),
                                       line));
            dest.availableBandwidth = DataRate(next);
            dest.alpha = (1.0 - m_rpAlphaGain) * dest.alpha + m_rpAlphaGain;
        }
        else
        {
            const uint64_t next =
                std::min<uint64_t>(dest.availableBandwidth.GetBitRate() + m_rpRateAi.GetBitRate(),
                                   line);
            dest.availableBandwidth = DataRate(next);
        }
        dest.congested = false;
    }

    for (uint32_t dst : destinations)
    {
        const DataRate bandwidth = GetOrCreateDestState(dst).availableBandwidth;
        std::vector<DpuFlowKey> keys;
        std::vector<DataRate> demands;
        for (const auto& flowEntry : m_flowStates)
        {
            if (flowEntry.first.destinationNodeId != dst)
            {
                continue;
            }
            keys.push_back(flowEntry.first);
            demands.push_back(flowEntry.second.demand);
        }
        if (keys.empty())
        {
            continue;
        }
        const std::vector<DataRate> alloc = Allocate(keys, demands, bandwidth);
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            m_flowStates[keys[i]].currentRate = alloc[i];
            if (m_rpPushEnabled)
            {
                PushRateToAsu(keys[i], alloc[i]);
            }
        }
    }

    m_allocEvent = Simulator::Schedule(m_rpUpdateInterval, &UbDpu::RecomputeAllocation, this);
}

void
UbDpu::OnPfcPause(uint32_t portId, uint8_t vl)
{
    if (m_ccRole != CcRole::RP || !m_rpPfcFeedbackEnabled)
    {
        return;
    }

    // Mark every flow at this VL as congested: the RoCE side just paused this VL, which is the
    // fastest CNP-independent congestion signal the DPU can observe.
    bool affected = false;
    for (const auto& entry : m_flowStates)
    {
        if (entry.first.vl == vl)
        {
            GetOrCreateDestState(entry.first.destinationNodeId).congested = true;
            affected = true;
        }
    }
    if (!affected)
    {
        return;
    }

    if (m_allocEvent.IsPending())
    {
        m_allocEvent.Cancel();
    }
    RecomputeAllocation();
}

bool
UbDpu::DirectForwardCtpDataPacket(Ptr<Packet> packet,
                                  const ParsedCtpHeaders& headers,
                                  uint32_t inPort,
                                  uint32_t outPort,
                                  uint32_t priority)
{
    if (headers.isCna24)
    {
        return false;
    }

    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return false;
    }
    Ptr<UbPort> inDevice = DynamicCast<UbPort>(node->GetDevice(inPort));
    if (inDevice == nullptr)
    {
        return false;
    }

    const uint8_t vl = headers.datalinkPacketHeader.GetPacketVL();
    const uint16_t scna = headers.cna16NetworkHeader.GetScna();
    const uint16_t dcna = headers.cna16NetworkHeader.GetDcna();

    // 网卡 QP 视图：发往 RoCE 侧的数据（response，scna=ASU → dcna=NPU）按 flow 入 QP 队列；
    // 反向（RoCE→UB，request/CNP）直接转回 UB 侧，不建 QP。
    if (IsRoceSide(outPort))
    {
        DpuFlowKey key{Cna16ToNodeId(scna), Cna16ToNodeId(dcna), vl};
        DpuFlowState& flow = GetOrCreateFlowState(key);
        flow.roceOutPort = outPort;

        if (IsPortCbfc(inPort))
        {
            UbFlowControlEventContext ctx;
            ctx.packet = packet;
            ctx.inPortId = inPort;
            ctx.outPortId = outPort;
            ctx.priority = priority;
            ctx.upstreamCreditAlreadyReturned = false;
            inDevice->GetFlowControl()->OnIngressReleased(ctx);
        }

        flow.pending.push(packet);
        TrySendQp(key);
        return true;
    }

    // RoCE→UB 方向（request/CNP 等）：直接透传回 UB 侧。
    if (IsPortCbfc(outPort))
    {
        Ptr<UbPort> outDevice = DynamicCast<UbPort>(node->GetDevice(outPort));
        if (outDevice == nullptr)
        {
            return false;
        }
        outDevice->EnqueueToEgress(std::make_tuple(inPort, priority, packet));
        outDevice->TriggerTransmit();
        return true;
    }

    return false;
}

void
UbDpu::TrySendQp(const DpuFlowKey& key)
{
    auto it = m_flowStates.find(key);
    if (it == m_flowStates.end())
    {
        return;
    }
    DpuFlowState& flow = it->second;
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return;
    }
    Ptr<UbPort> outDevice = DynamicCast<UbPort>(node->GetDevice(flow.roceOutPort));
    if (outDevice == nullptr)
    {
        return;
    }

    const uint64_t rateBps = std::max<uint64_t>(1u, flow.currentRate.GetBitRate());
    while (!flow.pending.empty())
    {
        Ptr<Packet> pkt = flow.pending.front();
        outDevice->EnqueueToEgress(std::make_tuple(flow.roceOutPort, key.vl, pkt));
        flow.pending.pop();

        const double seconds = static_cast<double>(pkt->GetSize()) * 8.0 /
                               static_cast<double>(rateBps);
        flow.nextSendTime = Simulator::Now() + Seconds(seconds);
    }
    outDevice->TriggerTransmit();
}

void
UbDpu::ExamineCtpDataPacket(Ptr<Packet> packet,
                            const ParsedCtpHeaders& headers,
                            uint32_t inPort,
                            uint32_t outPort,
                            Time* pacingDelay)
{
    *pacingDelay = Time(0);
    if (m_ccRole != CcRole::RP || headers.isCna24)
    {
        return;
    }

    const uint8_t vl = headers.datalinkPacketHeader.GetPacketVL();
    const uint16_t scna = headers.cna16NetworkHeader.GetScna();
    const uint16_t dcna = headers.cna16NetworkHeader.GetDcna();

    if (headers.ctpHeader.GetTPOpcode() == static_cast<uint8_t>(CtpOpcode::CTP_CNP))
    {
        // CNP travels NPU (scna) -> ASU (dcna). The DPU, as the sender's NIC, is its real
        // endpoint: record that the {ASU -> NPU} data flow is congested.
        DpuFlowKey key{Cna16ToNodeId(dcna), Cna16ToNodeId(scna), vl};
        OnCnpSeen(key);
        return;
    }

    // Request direction (RoCE -> UB): a read request from an NPU to an ASU. Pre-create the
    // reverse {ASU -> NPU} response flow so the allocator can assign it a rate even before its
    // first data packet arrives (breaks the victim starvation deadlock).
    if (IsRoceSide(inPort) && IsUbSide(outPort))
    {
        DpuFlowKey reverse{Cna16ToNodeId(dcna), Cna16ToNodeId(scna), vl};
        GetOrCreateFlowState(reverse);
        return;
    }

    if (!IsRoceSide(outPort) || packet == nullptr)
    {
        return;
    }

    DpuFlowKey key{Cna16ToNodeId(scna), Cna16ToNodeId(dcna), vl};
    OnDataPacket(key, packet->GetSize());

    if (!m_allocEvent.IsPending())
    {
        m_allocEvent = Simulator::Schedule(m_rpUpdateInterval, &UbDpu::RecomputeAllocation, this);
    }

    DpuFlowState& flow = GetOrCreateFlowState(key);
    const Time now = Simulator::Now();
    Time sendTime = now;
    if (flow.nextSendTime > now)
    {
        sendTime = flow.nextSendTime;
        *pacingDelay = flow.nextSendTime - now;
    }
    const uint64_t rateBps = std::max<uint64_t>(1u, flow.currentRate.GetBitRate());
    const double seconds =
        static_cast<double>(packet->GetSize()) * 8.0 / static_cast<double>(rateBps);
    flow.nextSendTime = sendTime + Seconds(seconds);
}

void
UbDpu::DoDispose()
{
    m_allocEvent.Cancel();
    m_flowStates.clear();
    m_destStates.clear();
    m_portSides.clear();
    UbSwitch::DoDispose();
}

} // namespace ns3