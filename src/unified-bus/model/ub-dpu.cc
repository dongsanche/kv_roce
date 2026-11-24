// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-dpu.h"
#include "ns3/ub-utils.h"

#include "ns3/ub-datatype.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-pfc.h"
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
            .AddAttribute("GradedBackpressure",
                          "When true, the DPU acts as a per-side CC terminus for graded hop-by-hop "
                          "backpressure: on receiving an FECN-marked ASU->NPU data packet (the union "
                          "marks it from its own local backlog), the DPU sends a local cut-CNP back "
                          "to the source ASU, so each hop runs its own local CC.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbDpu::m_gradedBackpressureEnabled),
                          MakeBooleanChecker())
            .AddAttribute("GradedCnpInterval",
                          "Minimum interval between local cut-CNPs sent to one flow when "
                          "GradedBackpressure is on.",
                          TimeValue(MicroSeconds(50)),
                          MakeTimeAccessor(&UbDpu::m_gradedCnpInterval),
                          MakeTimeChecker())
            .AddAttribute("PullMode",
                          "1825 doorbell pull: NPU READ/WRITE requests queue on this DPU (the "
                          "1825 request queue) and are actively doorbell-delivered to the "
                          "source ASU while the flow's in-flight data stays below "
                          "PullWindowBytes. The request delivery is the grant; the ASU side "
                          "carries no CBFC credit in this mode.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbDpu::m_pullMode),
                          MakeBooleanChecker())
            .AddAttribute("PullWindowBytes",
                          "Pull mode: per-flow in-flight window (bytes). Doorbells pause while "
                          "arrived-minus-forwarded data PLUS the response bytes promised by "
                          "already-doorbelled requests exceed this -- the 1825 pull speed that "
                          "bounds the union buffer.",
                          UintegerValue(1024 * 1024),
                          MakeUintegerAccessor(&UbDpu::m_pullWindowBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("PullRequestQueueCap",
                          "Pull mode: max queued request packets per flow (1825 request queue "
                          "capacity); excess requests are dropped and counted.",
                          UintegerValue(4096),
                          MakeUintegerAccessor(&UbDpu::m_pullRequestQueueCap),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RpPushEnabled",
                          "When true, the RP actively pushes its per-flow rate back to each "
                          "source ASU on every allocation update.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbDpu::m_rpPushEnabled),
                          MakeBooleanChecker())
            .AddAttribute("RpCqeFallbackEnabled",
                          "When true and RpPushEnabled is false, the RP additionally pushes the "
                          "per-flow rate to any ASU whose flow has not produced a completion (CQE) "
                          "for longer than RpCqeFallbackInterval, so stalled flows still receive "
                          "a rate update.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbDpu::m_rpCqeFallbackEnabled),
                          MakeBooleanChecker())
            .AddAttribute("RpCqeFallbackInterval",
                          "Minimum gap since the last observed CQE before the RP pushes a fallback "
                          "rate to a flow (only active when RpCqeFallbackEnabled is true).",
                          TimeValue(MicroSeconds(250)),
                          MakeTimeAccessor(&UbDpu::m_rpCqeFallbackInterval),
                          MakeTimeChecker())
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

UbDpu::DpuFlowState&
UbDpu::GetOrCreateFlowState(const DpuFlowKey& key)
{
    auto it = m_flowStates.find(key);
    if (it == m_flowStates.end())
    {
        DpuFlowState state;
        state.currentRate = m_rpInitialRate;
        state.nextSendTime = Simulator::Now();
        state.lastCqeTime = Simulator::Now();
        state.senderStarted = true;
        it = m_flowStates.emplace(key, std::move(state)).first;
    }
    return it->second;
}

void
UbDpu::OnCnpSeen(const DpuFlowKey& key)
{
    // A CNP is a congestion signal for exactly this {ASU -> NPU, VL} flow. Mark only it (not the
    // whole destination NPU) so a heavy flow's CNP never drags the victim's rate down. The
    // periodic pass applies the multiplicative decrease; we do not recompute immediately because
    // CNPs arrive at the CcCnpInterval and an immediate halving per CNP collapses too fast.
    GetOrCreateFlowState(key).congested = true;
}

void
UbDpu::PushRateToAsu(const DpuFlowKey& key, DataRate rate, CtpOpcode opcode)
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
    ctpHeader.SetTPOpcode(opcode);
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

void
UbDpu::SendCutCnp(const DpuFlowKey& key)
{
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return;
    }

    // Plain CTP_CNP (no UbCnpExtTph rate carrier): the ASU's CTP CC receives it with rate=0 and
    // applies the multiplicative decrease (OnCnpReceived). Sent from the DPU on behalf of the
    // destination NPU (scna=NPU, dcna=ASU) so the ASU's per-{NPU,VL} send rate is cut locally.
    Ptr<Packet> packet = Create<Packet>();

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

void
UbDpu::TryDoorbell(const DpuFlowKey& key)
{
    if (!m_pullMode)
    {
        return;
    }
    DpuFlowState& flow = GetOrCreateFlowState(key);
    if (flow.pendingRequests.empty())
    {
        return;
    }
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return;
    }
    // The request delivery IS the pull grant: doorbell the 1825 request queue to the ASU
    // while the flow's in-flight load (arrived-minus-forwarded data PLUS the response bytes
    // already promised by doorbelled requests) stays below the pull window. The reservation
    // is what prevents an emptied window from dumping the whole request backlog at once and
    // flooding the union with a burst that bypasses the window.
    while (!flow.pendingRequests.empty() &&
           flow.pullOutstandingBytes + flow.pullDoorbellReserved < m_pullWindowBytes)
    {
        const PendingPacket& front = flow.pendingRequests.front();
        Ptr<UbPort> outDevice = DynamicCast<UbPort>(node->GetDevice(front.inPortId));
        if (outDevice == nullptr)
        {
            break;
        }
        if (!outDevice->EnqueueToEgress(std::make_tuple(front.inPortId, key.vl, front.packet)))
        {
            break; // UB egress full: hold the doorbell, retry on the next drain/arrival
        }
        outDevice->TriggerTransmit();
        // One request segment yields one response segment (observed 2048 requests per 8MB
        // read): reserve a full MTU per doorbell and settle it as the data actually arrives.
        flow.pullDoorbellReserved += UB_MTU_BYTE;
        flow.pendingRequests.pop();
    }
}

void
UbDpu::RecomputeAllocation()
{
    const DataRate line = DeriveRoceLineRate();
    const uint64_t lineBps = line.GetBitRate();
    const uint64_t aiBps = m_rpRateAi.GetBitRate();
    const Time now = Simulator::Now();

    for (auto& entry : m_flowStates)
    {
        const DpuFlowKey& key = entry.first;
        DpuFlowState& flow = entry.second;

        const uint64_t cur = flow.currentRate.GetBitRate();
        uint64_t next = cur;
        if (flow.congested)
        {
            // Multiplicative decrease on this flow only, so a heavy flow's CNP never drags the
            // victim's rate down with it.
            const double cutFactor = 1.0 - flow.alpha / 2.0;
            next = std::max<uint64_t>(
                1u, std::min<uint64_t>(static_cast<uint64_t>(static_cast<double>(cur) * cutFactor),
                                       lineBps));
            flow.alpha = (1.0 - m_rpAlphaGain) * flow.alpha + m_rpAlphaGain;
        }
        else
        {
            next = std::min<uint64_t>(cur + aiBps, lineBps);
        }
        flow.currentRate = DataRate(next);
        flow.congested = false;

        if (m_rpPushEnabled)
        {
            PushRateToAsu(key, flow.currentRate);
        }
        else if (m_rpCqeFallbackEnabled && now - flow.lastCqeTime >= m_rpCqeFallbackInterval)
        {
            // No completion for this flow recently: push its current AIMD rate anyway so a
            // stalled flow is not left without any rate update.
            PushRateToAsu(key, flow.currentRate);
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
    for (auto& entry : m_flowStates)
    {
        if (entry.first.vl == vl)
        {
            entry.second.congested = true;
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

        // Pull mode arrival accounting: this WQE's data has entered the DPU. The burst flag
        // arms the next token issue once the burst goes quiet AND everything has been
        // forwarded (CheckPullQuiet) -- that is the 1825 pull-speed bound on the union.
        if (m_pullMode)
        {
            flow.pullOutstandingBytes += packet->GetSize();
            // Settle the doorbell reservation: the promised response bytes have arrived.
            const uint64_t arrived = packet->GetSize();
            flow.pullDoorbellReserved =
                flow.pullDoorbellReserved > arrived ? flow.pullDoorbellReserved - arrived : 0;
        }

        // Start the periodic allocation loop on the first observed data packet, so rate control
        // works without relying on PFC feedback.
        if (m_ccRole == CcRole::RP && !m_allocEvent.IsPending())
        {
            m_allocEvent =
                Simulator::Schedule(m_rpUpdateInterval, &UbDpu::RecomputeAllocation, this);
        }

        // Graded hop-by-hop backpressure (scheme 3): the union marks FECN on this ASU->NPU data
        // from its own local backlog; the DPU (as the UB-side receiver) turns that into a local
        // cut-CNP back to the source ASU, so the ASU's CTP CC slows locally (each hop CC).
        if (m_gradedBackpressureEnabled && headers.cna16NetworkHeader.GetFecn() != 0)
        {
            const Time gradedNow = Simulator::Now();
            if (flow.lastCutCnpTime.IsZero() ||
                gradedNow - flow.lastCutCnpTime >= m_gradedCnpInterval)
            {
                flow.lastCutCnpTime = gradedNow;
                SendCutCnp(key);
            }
        }

        // Hold the packet in the per-flow QP. The UB-side CBFC credit is NOT returned here; it is
        // returned only once TrySendQp actually forwards the packet to the RoCE egress, so a
        // PFC-paused / full egress backpressures the upstream ASU instead of dropping.
        // Small packets (TAACKs / CNP echoes) take the unscheduled lane so acks never queue
        // behind the data backlog (otherwise PFC backpressure starves the request pump).
        if (packet->GetSize() <= 256)
        {
            flow.pendingControl.push(PendingPacket{packet, inPort});
        }
        else
        {
            flow.pending.push(PendingPacket{packet, inPort});
        }
        TrySendQp(key);
        if (!m_retryEvent.IsPending())
        {
            m_retryEvent = Simulator::Schedule(m_rpUpdateInterval, &UbDpu::RetryHeldFlows, this);
        }
        return true;
    }

    // RoCE→UB 方向（request/CNP/CQE 等）：透传回 UB 侧，并顺带观测控制帧。
    if (IsPortCbfc(outPort))
    {
        // Scheme 3 (graded) ONLY, gated on GradedBackpressure + TRANSPARENT (schemes 1/2 untouched):
        // translate the RoCE-domain CNP into a per-VL UB-side CNP and re-emit it to the ASU
        // (CTP CC is per-VL), consuming the original.
        if (m_gradedBackpressureEnabled && m_ccRole == CcRole::TRANSPARENT &&
            headers.ctpHeader.GetTPOpcode() == static_cast<uint8_t>(CtpOpcode::CTP_CNP))
        {
            SendCutCnp(DpuFlowKey{Cna16ToNodeId(dcna), Cna16ToNodeId(scna), vl});
            return true;
        }

        if (m_ccRole == CcRole::RP)
        {
            const uint8_t tpOpcode = headers.ctpHeader.GetTPOpcode();
            if (tpOpcode == static_cast<uint8_t>(CtpOpcode::CTP_CNP))
            {
                // The DPU is the reaction point: consume the end-to-end CNP instead of passing
                // it through, so the ASU's own DCQCN does not cut the rate a second time on top
                // of the RP's allocation. Mark the {ASU -> NPU} flow congested; the allocator
                // picks it up on the next periodic pass.
                OnCnpSeen(DpuFlowKey{Cna16ToNodeId(dcna), Cna16ToNodeId(scna), vl});
                return true;
            }
            else if (tpOpcode == static_cast<uint8_t>(CtpOpcode::CTP_CQE))
            {
                // Read completion back to the ASU: rewrite the rate field with this flow's
                // current allocation and forward the updated CQE (the responder rate-limits from
                // it). Consume the original so only the rate-bearing CQE reaches the ASU.
                DpuFlowKey key{Cna16ToNodeId(dcna), Cna16ToNodeId(scna), vl};
                auto flowIt = m_flowStates.find(key);
                if (flowIt != m_flowStates.end())
                {
                    flowIt->second.lastCqeTime = Simulator::Now();
                    PushRateToAsu(key, flowIt->second.currentRate, CtpOpcode::CTP_CQE);
                }
                return true;
            }
        }

        Ptr<UbPort> outDevice = DynamicCast<UbPort>(node->GetDevice(outPort));
        if (outDevice == nullptr)
        {
            return false;
        }
        // 1825 request queue + active doorbell (pull mode): NPU READ/WRITE requests
        // (CTP_DATA) queue on this DPU and are doorbell-delivered to the ASU under the
        // pull-window gate; CNP/CQE/PULL control packets pass through immediately
        // (latency-critical). The request delivery itself is the pull grant.
        if (m_pullMode && headers.ctpHeader.GetTPOpcode() == static_cast<uint8_t>(CtpOpcode::CTP_DATA))
        {
            const DpuFlowKey pullKey{Cna16ToNodeId(dcna), Cna16ToNodeId(scna), vl};
            DpuFlowState& pullFlow = GetOrCreateFlowState(pullKey);
            // Record the UB egress port in the pending entry; TryDoorbell forwards there.
            if (pullFlow.pendingRequests.size() >= m_pullRequestQueueCap)
            {
                // 1825 request queue full: the NPU's outstanding window exceeds this DPU's
                // request buffering. Count it (a real device would NAK / withhold SQ credit).
                ++pullFlow.pullRequestDrops;
                utils::UbUtils::RecordRuntimePacketDrop("1825 request queue full");
                return true;
            }
            pullFlow.pendingRequests.push(PendingPacket{packet, outPort});
            TryDoorbell(pullKey);
            return true;
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

    // If the RoCE egress VL is currently PFC-paused, hold the whole QP (no credit returned, so the
    // upstream ASU is backpressured rather than allowed to keep flooding the switch).
    if (IsRoceVlPaused(flow.roceOutPort, key.vl))
    {
        return;
    }

    Ptr<UbEgressQueue> egress = outDevice->GetUbQueue();
    const uint64_t rateBps = std::max<uint64_t>(1u, flow.currentRate.GetBitRate());
    bool sent = false;
    // Unscheduled lane first: small packets (TAACKs / CNP echoes) drain ahead of the data
    // backlog so acks survive PFC backpressure (request-pump starvation otherwise deadlocks
    // pull mode's request-driven response generation).
    while (!flow.pendingControl.empty())
    {
        const PendingPacket& front = flow.pendingControl.front();
        if (egress != nullptr && !egress->CanEnqueue(front.packet->GetSize()))
        {
            break; // egress queue byte limit reached: hold, retry later
        }
        if (!outDevice->EnqueueToEgress(std::make_tuple(flow.roceOutPort, key.vl, front.packet)))
        {
            break; // egress rejected the packet: hold
        }
        Ptr<UbPort> inDevice = DynamicCast<UbPort>(node->GetDevice(front.inPortId));
        if (inDevice != nullptr && IsPortCbfc(front.inPortId))
        {
            UbFlowControlEventContext ctx;
            ctx.packet = front.packet;
            ctx.inPortId = front.inPortId;
            ctx.outPortId = flow.roceOutPort;
            ctx.priority = key.vl;
            ctx.upstreamCreditAlreadyReturned = false;
            inDevice->GetFlowControl()->OnIngressReleased(ctx);
        }
        if (m_pullMode)
        {
            flow.pullOutstandingBytes = flow.pullOutstandingBytes > front.packet->GetSize()
                                            ? flow.pullOutstandingBytes - front.packet->GetSize()
                                            : 0;
        }
        flow.pendingControl.pop();
        sent = true;
    }
    while (!flow.pending.empty())
    {
        const PendingPacket& front = flow.pending.front();
        if (egress != nullptr && !egress->CanEnqueue(front.packet->GetSize()))
        {
            break; // egress queue byte limit reached: hold, retry later
        }
        if (!outDevice->EnqueueToEgress(std::make_tuple(flow.roceOutPort, key.vl, front.packet)))
        {
            break; // egress rejected the packet: hold
        }

        // The packet actually left the NIC toward the RoCE side, so return the UB-side CBFC credit
        // to the upstream ASU now (this is the point where backpressure is released).
        Ptr<UbPort> inDevice = DynamicCast<UbPort>(node->GetDevice(front.inPortId));
        if (inDevice != nullptr && IsPortCbfc(front.inPortId))
        {
            UbFlowControlEventContext ctx;
            ctx.packet = front.packet;
            ctx.inPortId = front.inPortId;
            ctx.outPortId = flow.roceOutPort;
            ctx.priority = key.vl;
            ctx.upstreamCreditAlreadyReturned = false;
            inDevice->GetFlowControl()->OnIngressReleased(ctx);
        }

        // Pull mode: the flow's in-flight data drained toward the RoCE wire; the pull window
        // re-opens (doorbell re-fires after this loop).
        if (m_pullMode)
        {
            flow.pullOutstandingBytes = flow.pullOutstandingBytes > front.packet->GetSize()
                                            ? flow.pullOutstandingBytes - front.packet->GetSize()
                                            : 0;
        }
        if (flow.kvPushActive)
        {
            // KV push: return consumed bytes to the ASU in batches (>=64KB) so it can push more.
            flow.kvCreditAccum += front.packet->GetSize();
            if (flow.kvCreditAccum >= 64 * 1024)
            {
                SendKvCredit(key, flow.kvCreditAccum, front.inPortId);
                flow.kvCreditAccum = 0;
            }
        }
        const double seconds =
            static_cast<double>(front.packet->GetSize()) * 8.0 / static_cast<double>(rateBps);
        flow.nextSendTime = Simulator::Now() + Seconds(seconds);
        flow.pending.pop();
        sent = true;
    }
    if (sent)
    {
        outDevice->TriggerTransmit();
    }
    if (m_pullMode)
    {
        // Data left the flow: the pull window opened, doorbell queued requests again.
        TryDoorbell(key);
    }
}

bool
UbDpu::DirectForwardKvFrame(Ptr<Packet> packet, uint32_t inPort, uint32_t outPort)
{
    static const bool kvPush = []() {
        BooleanValue enabled;
        return GlobalValue::GetValueByNameFailSafe("UB_KV_PUSH_MODE", enabled) && enabled.Get();
    }();
    if (!kvPush)
    {
        return false;
    }
        Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return false;
    }
    Ptr<Packet> view = packet->Copy();
    UbDatalinkPacketHeader dlHeader;
    UbCna16NetworkHeader cnaHeader;
    UbKvHeader kvHeader;
    view->RemoveHeader(dlHeader);
    view->RemoveHeader(cnaHeader);
    view->RemoveHeader(kvHeader);
    const uint8_t vl = dlHeader.GetPacketVL();
    const uint32_t dcna = utils::Cna16ToNodeId(cnaHeader.GetDcna());
    const uint32_t scna = utils::Cna16ToNodeId(cnaHeader.GetScna());

    if (IsRoceSide(inPort))
    {
        // NPU -> ASU request: the flow key is {ASU, NPU, vl} (ASU = destination CNA).
        const DpuFlowKey key{dcna, scna, vl};
        DpuFlowState& flow = GetOrCreateFlowState(key);
        // NPU -> ASU: the READ request enters the 1825 request queue and is doorbelled
        // (one active push per flow; the push credits pace the data, not the request).
        if (kvHeader.GetOpcode() != static_cast<uint8_t>(KvOpcode::KV_READ_REQ))
        {
            return false;
        }
        if (flow.pendingRequests.size() >= m_pullRequestQueueCap)
        {
            ++flow.pullRequestDrops;
            utils::UbUtils::RecordRuntimePacketDrop("1825 KV request queue full");
            return true;
        }
        flow.pendingRequests.push(PendingPacket{packet, outPort});
        TryKvDoorbell(key);
        return true;
    }
    if (IsPortCbfc(inPort))
    {
        // ASU -> NPU: the flow key is again {ASU, NPU, vl} (ASU = source CNA here).
        const DpuFlowKey key{scna, dcna, vl};
        DpuFlowState& flow = GetOrCreateFlowState(key);
        // Push data (forward via the per-flow QP) or the WQE-done marker.
        if (kvHeader.GetOpcode() == static_cast<uint8_t>(KvOpcode::KV_DATA))
        {
            flow.roceOutPort = outPort;
            flow.pending.push(PendingPacket{packet, inPort});
            TrySendQp(key);
            if (!m_retryEvent.IsPending())
            {
                m_retryEvent = Simulator::Schedule(m_rpUpdateInterval, &UbDpu::RetryHeldFlows, this);
            }
            return true;
        }
        if (kvHeader.GetOpcode() == static_cast<uint8_t>(KvOpcode::KV_DONE))
        {
            flow.pending.push(PendingPacket{packet, inPort});
            TrySendQp(key);
            flow.kvPushActive = false;
            if (flow.kvCreditAccum > 0)
            {
                SendKvCredit(key, flow.kvCreditAccum, inPort);
                flow.kvCreditAccum = 0;
            }
            TryKvDoorbell(key);
            return true;
        }
        return false;
    }
    return false;
}

void
UbDpu::TryKvDoorbell(const DpuFlowKey& key)
{
    DpuFlowState& flow = GetOrCreateFlowState(key);
    if (flow.kvPushActive || flow.pendingRequests.empty())
    {
        return;
    }
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return;
    }
    const PendingPacket& front = flow.pendingRequests.front();
    const uint32_t ubPort = front.inPortId;
    Ptr<UbPort> outDev = DynamicCast<UbPort>(node->GetDevice(ubPort));
    if (outDev == nullptr)
    {
        return;
    }
    if (!outDev->EnqueueToEgress(std::make_tuple(ubPort, key.vl, front.packet)))
    {
        return;
    }
    outDev->TriggerTransmit();
    flow.pendingRequests.pop();
    flow.kvPushActive = true;
    flow.kvCreditAccum = 0;
    // Initial push window: the ASU may push this many bytes before waiting for credits.
    SendKvCredit(key, m_pullWindowBytes, ubPort);
}

void
UbDpu::SendKvCredit(const DpuFlowKey& key, uint64_t bytes, uint32_t ubPort)
{
    if (bytes == 0)
    {
        return;
    }
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr || !IsUbSide(ubPort))
    {
        return;
    }
    Ptr<Packet> packet = Create<Packet>();
    UbKvHeader kvHeader;
    kvHeader.SetOpcode(static_cast<uint8_t>(KvOpcode::KV_CREDIT));
    kvHeader.SetVl(key.vl);
    kvHeader.SetKvAddr(bytes); // credit bytes travel in the 64-bit address field
    packet->AddHeader(kvHeader);
    UbCna16NetworkHeader cnaHeader;
    cnaHeader.SetScna(static_cast<uint16_t>(utils::NodeIdToCna16(key.destinationNodeId)));
    cnaHeader.SetDcna(static_cast<uint16_t>(utils::NodeIdToCna16(key.sourceNodeId)));
    cnaHeader.SetServiceLevel(key.vl);
    cnaHeader.SetNlp(UB_CNA_NLP_KVPUSH);
    packet->AddHeader(cnaHeader);
    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                key.vl,
                                key.vl,
                                RoutingType::PER_FLOW_SHORTEST_PATHS,
                                UbDatalinkHeaderConfig::PACKET_CNA16);
    SendPacket(packet, ubPort, ubPort, key.vl);
}

void
UbDpu::RetryHeldFlows()
{
    for (auto& entry : m_flowStates)
    {
        TrySendQp(entry.first);
        TryDoorbell(entry.first);
    }
    m_retryEvent = Simulator::Schedule(m_rpUpdateInterval, &UbDpu::RetryHeldFlows, this);
}

bool
UbDpu::IsRoceVlPaused(uint32_t portId, uint8_t vl) const
{
    Ptr<Node> node = GetObject<Node>();
    if (node == nullptr)
    {
        return false;
    }
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
    if (port == nullptr)
    {
        return false;
    }
    Ptr<UbPfc> pfc = DynamicCast<UbPfc>(port->GetFlowControl());
    return pfc != nullptr && pfc->IsVlPaused(vl);
}

void
UbDpu::ExamineCtpDataPacket(Ptr<Packet> packet,
                            const ParsedCtpHeaders& headers,
                            uint32_t inPort,
                            uint32_t outPort,
                            Time* pacingDelay)
{
    (void)packet;
    (void)headers;
    (void)inPort;
    (void)outPort;
    // DPU rate control happens in DirectForwardCtpDataPacket (per-flow AIMD + rate delivery to
    // the ASU). No egress pacing here; the ASU honors the pushed/CQE-carried rate itself.
    *pacingDelay = Time(0);
}

void
UbDpu::DoDispose()
{
    m_allocEvent.Cancel();
    m_retryEvent.Cancel();
    m_flowStates.clear();
    m_portSides.clear();
    UbSwitch::DoDispose();
}

} // namespace ns3