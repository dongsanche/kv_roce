// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_SSD_H
#define UB_SSD_H

#include "ns3/callback.h"
#include "ns3/data-rate.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/type-id.h"

#include <cstdint>
#include <queue>
#include <vector>

namespace ns3 {

/**
 * @brief ASU 侧 SSD 存储模型。
 *
 * 把 ASU 的存储访问从「0 延迟、无限带宽」改成显式的 SSD 服务：按 Jetty 分队列，
 * WRR 按请求个数调度，每个事务服务时间 = latency + bytes / bandwidth。SSD 是单一
 * 服务器，同一时刻只服务一个请求，因此 bandwidth 即聚合吞吐上限。
 */
class UbSsd : public Object
{
  public:
    static TypeId GetTypeId();
    UbSsd();
    ~UbSsd() override;

    void SetReadLatency(Time t);
    void SetWriteLatency(Time t);
    void SetBandwidth(DataRate r);
    void SetQueueCount(uint32_t n);
    void SetQueueWeights(const std::vector<uint32_t>& weights);

    Time GetReadLatency() const;
    Time GetWriteLatency() const;
    DataRate GetBandwidth() const;

    /**
     * 提交一个读事务（jettyNum 决定队列，bytes 决定带宽项）。
     * 当 SSD 服务完（(chargeLatency ? latency : 0) + bytes/bandwidth，经 WRR 串行）
     * 后触发 done。chargeLatency 用于让一次读/写的固定 latency 只按事务收一次，
     * 而不是按 TA segment 重复收。
     */
    void SubmitRead(uint32_t jettyNum, uint64_t bytes, Callback<void> done, bool chargeLatency = true);

    /**
     * 提交一个写事务；服务时间用 write latency。
     */
    void SubmitWrite(uint32_t jettyNum,
                     uint64_t bytes,
                     Callback<void> done,
                     bool chargeLatency = true);

  protected:
    void DoDispose() override;

  private:
    struct Request
    {
        uint64_t bytes{0};
        Callback<void> done;
        bool isWrite{false};
        bool chargeLatency{true};
    };

    Time m_readLatency;
    Time m_writeLatency;
    DataRate m_bandwidth;
    uint32_t m_queueCount{1};
    std::vector<uint32_t> m_weights;
    std::vector<std::queue<Request>> m_queues;

    // WRR + 串行状态
    uint32_t m_roundIdx{0};
    uint32_t m_servedInQueue{0};
    Time m_busyUntil{0};
    bool m_servicePending{false};

    void Submit(uint32_t jettyNum,
                uint64_t bytes,
                Callback<void> done,
                bool isWrite,
                bool chargeLatency);
    void ScheduleService();
    void ServiceNext();
    bool PickNext(uint32_t& queueIdx, Request& req);
    uint32_t MapJettyToQueue(uint32_t jettyNum) const;
};

} // namespace ns3

#endif // UB_SSD_H
