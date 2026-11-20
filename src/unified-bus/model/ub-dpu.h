// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_DPU_H
#define UB_DPU_H

#include "ns3/ub-switch.h"

#include "ns3/data-rate.h"
#include "ns3/event-id.h"

#include <cstdint>
#include <map>
#include <queue>
#include <vector>

namespace ns3 {

/**
 * @brief 独立 DPU/网卡节点（如 1825）的数据面。
 *
 * DPU 是 UB 域和 RoCE 域之间的网卡，不是交换机：
 * - 端口流控按端口侧归置：UB 侧 CBFC、RoCE 侧 PFC；
 * - 端口侧由 node.csv 的 rocePortCount 决定（末尾 rocePortCount 个端口为 RoCE 侧，
 *   其余为 UB 侧，与 UbGateway1825 的端口分侧约定一致）。
 *
 * 拥塞控制角色由 CcRole 属性决定：
 * - TRANSPARENT（默认）：纯透明转发，不参与端到端拥塞控制（IsCongestionMarkingEnabled = false），
 *   限速点在上游主机（ASU）的 CTP CC。
 * - RP：DPU 作为 RoCE 反应点（限速点）。它对每条 {源 ASU, 目的 NPU, VL} 流独立做 AIMD 控速：
 *   观测该流的 CNP（乘性减）或长时间无拥塞信号（加性增），把算出的 rate 写到 CQE 字段发回
 *   ASU，ASU 据此调速。拥塞状态按流维护，一条流拥塞不会连累同 NPU 下的其它流。
 */
class UbDpu : public UbSwitch
{
  public:
    enum class PortSide : uint8_t
    {
        UB = 0,
        ROCE = 1,
    };

    enum class CcRole : uint8_t
    {
        TRANSPARENT = 0,
        RP = 1,
    };

    UbDpu();
    ~UbDpu() override;

    static TypeId GetTypeId();

    void SetPortSide(uint32_t portId, PortSide side);
    PortSide GetPortSide(uint32_t portId) const;
    bool IsUbSide(uint32_t portId) const;
    bool IsRoceSide(uint32_t portId) const;

    FcType GetFlowControlTypeForPort(uint32_t portId) const override;
    bool IsCongestionMarkingEnabled() const override { return false; }

  protected:
    void ExamineCtpDataPacket(Ptr<Packet> packet,
                              const ParsedCtpHeaders& headers,
                              uint32_t inPort,
                              uint32_t outPort,
                              Time* pacingDelay) override;
    void OnPfcPause(uint32_t portId, uint8_t vl) override;
    bool DirectForwardCtpDataPacket(Ptr<Packet> packet,
                                    const ParsedCtpHeaders& headers,
                                    uint32_t inPort,
                                    uint32_t outPort,
                                    uint32_t priority) override;
    void DoDispose() override;

  private:
    struct DpuFlowKey
    {
        uint32_t sourceNodeId{0};
        uint32_t destinationNodeId{0};
        uint8_t vl{0};

        bool operator<(const DpuFlowKey& other) const
        {
            if (sourceNodeId != other.sourceNodeId)
            {
                return sourceNodeId < other.sourceNodeId;
            }
            if (destinationNodeId != other.destinationNodeId)
            {
                return destinationNodeId < other.destinationNodeId;
            }
            return vl < other.vl;
        }
    };

    struct DpuFlowState
    {
        DataRate currentRate{0};
        double alpha{1.0};
        bool congested{false};
        Time nextSendTime{0};
        Time lastCqeTime{0};
        bool senderStarted{false};
        uint32_t roceOutPort{0};
        std::queue<Ptr<Packet>> pending;
    };

    DataRate DeriveRoceLineRate() const;
    DpuFlowState& GetOrCreateFlowState(const DpuFlowKey& key);
    void OnCnpSeen(const DpuFlowKey& key);
    void PushRateToAsu(const DpuFlowKey& key, DataRate rate, CtpOpcode opcode = CtpOpcode::CTP_CNP);
    void TrySendQp(const DpuFlowKey& key);
    void RecomputeAllocation();

    CcRole m_ccRole{CcRole::TRANSPARENT};
    bool m_rpPushEnabled{false};
    bool m_rpPfcFeedbackEnabled{true};
    bool m_rpCqeFallbackEnabled{false};
    DataRate m_rpLineRate{"400Gbps"};
    DataRate m_rpInitialRate{"400Gbps"};
    DataRate m_rpRateAi{"100Mbps"};
    Time m_rpUpdateInterval{MicroSeconds(55)};
    Time m_rpCqeFallbackInterval{MicroSeconds(250)};
    double m_rpAlphaGain{0.00390625};
    uint64_t m_rpBacklogThreshold{65536};

    std::map<DpuFlowKey, DpuFlowState> m_flowStates;
    EventId m_allocEvent;

    FcType m_ubFlowControlType{FcType::CBFC};
    FcType m_roceFlowControlType{FcType::PFC_DYNAMIC_PAPER};
    std::vector<PortSide> m_portSides;
};

} // namespace ns3

#endif // UB_DPU_H
