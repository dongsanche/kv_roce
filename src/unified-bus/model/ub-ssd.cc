// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-ssd.h"

#include "ns3/boolean.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbSsd");
NS_OBJECT_ENSURE_REGISTERED(UbSsd);

TypeId
UbSsd::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbSsd")
            .SetParent<Object>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbSsd>()
            .AddAttribute("ReadLatency",
                          "Fixed SSD read latency per transaction.",
                          TimeValue(MicroSeconds(50)),
                          MakeTimeAccessor(&UbSsd::SetReadLatency, &UbSsd::GetReadLatency),
                          MakeTimeChecker())
            .AddAttribute("WriteLatency",
                          "Fixed SSD write latency per transaction.",
                          TimeValue(MicroSeconds(80)),
                          MakeTimeAccessor(&UbSsd::SetWriteLatency, &UbSsd::GetWriteLatency),
                          MakeTimeChecker())
            .AddAttribute("Bandwidth",
                          "SSD sequential bandwidth (PCIe 5.0 x4 NVMe ~= 12 GB/s).",
                          DataRateValue(DataRate("12GB/s")),
                          MakeDataRateAccessor(&UbSsd::SetBandwidth, &UbSsd::GetBandwidth),
                          MakeDataRateChecker())
            .AddAttribute("QueueCount",
                          "Number of SSD queues (Jets are mapped by jettyNum % QueueCount).",
                          UintegerValue(8),
                          MakeUintegerAccessor(&UbSsd::SetQueueCount),
                          MakeUintegerChecker<uint32_t>(1, 256));
    return tid;
}

UbSsd::UbSsd()
    : m_readLatency(MicroSeconds(50)),
      m_writeLatency(MicroSeconds(80)),
      m_bandwidth(DataRate("12GB/s")),
      m_queueCount(8),
      m_weights(8, 1),
      m_queues(8)
{
}

UbSsd::~UbSsd() = default;

void
UbSsd::SetReadLatency(Time t)
{
    m_readLatency = t;
}

void
UbSsd::SetWriteLatency(Time t)
{
    m_writeLatency = t;
}

void
UbSsd::SetBandwidth(DataRate r)
{
    m_bandwidth = r;
}

void
UbSsd::SetQueueCount(uint32_t n)
{
    m_queueCount = std::max<uint32_t>(1, n);
    m_queues.resize(m_queueCount);
    if (m_weights.size() < m_queueCount)
    {
        m_weights.resize(m_queueCount, 1);
    }
}

void
UbSsd::SetQueueWeights(const std::vector<uint32_t>& weights)
{
    m_weights = weights;
    if (m_weights.empty())
    {
        m_weights.assign(m_queueCount, 1);
    }
    if (m_weights.size() < m_queueCount)
    {
        m_weights.resize(m_queueCount, 1);
    }
    for (auto& w : m_weights)
    {
        w = std::max<uint32_t>(1, w);
    }
}

Time
UbSsd::GetReadLatency() const
{
    return m_readLatency;
}

Time
UbSsd::GetWriteLatency() const
{
    return m_writeLatency;
}

DataRate
UbSsd::GetBandwidth() const
{
    return m_bandwidth;
}

uint32_t
UbSsd::MapJettyToQueue(uint32_t jettyNum) const
{
    return jettyNum % m_queueCount;
}

void
UbSsd::SubmitRead(uint32_t jettyNum, uint64_t bytes, Callback<void> done, bool chargeLatency)
{
    Submit(jettyNum, bytes, done, false, chargeLatency);
}

void
UbSsd::SubmitWrite(uint32_t jettyNum, uint64_t bytes, Callback<void> done, bool chargeLatency)
{
    Submit(jettyNum, bytes, done, true, chargeLatency);
}

void
UbSsd::Submit(uint32_t jettyNum, uint64_t bytes, Callback<void> done, bool isWrite, bool chargeLatency)
{
    uint32_t q = MapJettyToQueue(jettyNum);
    Request req;
    req.bytes = bytes;
    req.done = done;
    req.isWrite = isWrite;
    req.chargeLatency = chargeLatency;
    m_queues[q].push(req);
    ScheduleService();
}

void
UbSsd::ScheduleService()
{
    if (m_servicePending)
    {
        return;
    }
    m_servicePending = true;
    Simulator::ScheduleNow(&UbSsd::ServiceNext, this);
}

bool
UbSsd::PickNext(uint32_t& queueIdx, Request& req)
{
    for (uint32_t attempts = 0; attempts < m_queueCount; ++attempts)
    {
        uint32_t q = m_roundIdx;
        if (!m_queues[q].empty())
        {
            req = m_queues[q].front();
            m_queues[q].pop();
            queueIdx = q;
            ++m_servedInQueue;
            if (m_servedInQueue >= m_weights[q])
            {
                m_servedInQueue = 0;
                m_roundIdx = (m_roundIdx + 1) % m_queueCount;
            }
            return true;
        }
        m_servedInQueue = 0;
        m_roundIdx = (m_roundIdx + 1) % m_queueCount;
    }
    return false;
}

void
UbSsd::ServiceNext()
{
    m_servicePending = false;

    uint32_t queueIdx = 0;
    Request req;
    if (!PickNext(queueIdx, req))
    {
        return;
    }

    const Time now = Simulator::Now();
    const Time start = std::max(now, m_busyUntil);

    const double transferSeconds =
        static_cast<double>(req.bytes) * 8.0 / static_cast<double>(m_bandwidth.GetBitRate());
    const Time latency =
        req.chargeLatency ? (req.isWrite ? m_writeLatency : m_readLatency) : Time(0);
    const Time finish = start + latency + Seconds(transferSeconds);
    m_busyUntil = finish;

    Callback<void> done = req.done;
    Simulator::Schedule(finish - now, done);

    ScheduleService();
}

void
UbSsd::DoDispose()
{
    for (auto& q : m_queues)
    {
        while (!q.empty())
        {
            q.pop();
        }
    }
    Object::DoDispose();
}

} // namespace ns3
