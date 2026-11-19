# ASU SSD 存储模型设计

> 目标：给 ASU（1815e NIC + SSD）补上**存储侧**建模。当前 ASU 的存储访问是 0 延迟、
> 无限带宽（数据直接以网卡线速往外推），会低估 ASU 侧时延、把瓶颈错误算在网络侧。
> 本设计引入「按 Jetty 分队列 + WRR 调度 + SSD 读写延迟/带宽参数」，使 SSD 成为 ASU
> 侧的显式瓶颈（PCIe 5.0 的 ~12GB/s << 网络 200G）。

## 1. 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `ssd-read-latency` | 50 μs | 每个读事务的固定延迟（SSD 控制器 + NAND 访问）|
| `ssd-write-latency` | 80 μs | 每个写事务的固定延迟 |
| `ssd-bandwidth` | 12 GB/s | 单块 SSD 顺序读写带宽（PCIe 5.0 x4 NVMe 量级）|
| `ssd-count` | 5 | 每 ASU 的 SSD 块数；聚合带宽 = `ssd-count × ssd-bandwidth` |
| `ssd-queue-count` | 8 | SSD 队列数（与 Jetty 对齐，见 §3）|
| `ssd-queue-weights` | `"1 1 1 1 1 1 1 1"` | 各队列 WRR 权重（请求个数/轮）|

### PCIe 5.0 例值

- `ssd-bandwidth = 12GB/s`（PCIe 5.0 x4 顺序读）。
- `ssd-read-latency = 50μs`、`ssd-write-latency = 80μs`。
- `ssd-count = 5` → 聚合带宽 `60GB/s`，对标 400G 网卡（~50GB/s）量级，避免 SSD 带宽
  成为唯一的人为瓶颈；`ssd-count = 1` 时回到单块 12GB/s。

一个 4MB 读事务的 SSD 服务时间 ≈ `50μs + 4MB/bandwidth`：
- `ssd-count=1`（12GB/s）：`50μs + 333μs ≈ 383μs` → ASU 聚合读吞吐被卡在 ~12GB/s，
  远低于网卡 200G（25GB/s）→ SSD 是读方向真瓶颈。
- `ssd-count=5`（60GB/s）：`50μs + 67μs ≈ 117μs` → SSD 不再主导，瓶颈回到网络侧。

只有事务很小（< ~100KB）时，latency 才主导；大 KV 块是 bandwidth 主导。

## 2. 队列模型：按 Jetty 分

`UbJetty` 是「一个源-目的对的 WQE 队列 + 传输状态」（见 `ub-function.h` 注释）。一个
read/write 事务在一个 Jetty 上产生 WQE/segment。

- **每个 Jetty 映射到一个 SSD 队列**：`queue_id = jettyNum % ssd_queue_count`（Jetty
  数量超过队列数时取模；默认 8 队列）。
- 队列是**请求级**的 FIFO：同一个 Jetty 的请求先进先出，跨 Jetty 由 WRR 仲裁（§3）。
- 好处：直接复用现有 Jetty 作为「存储流」的载体，不做新的流抽象；多租户/多源并发时
  每个源-目的对天然是一个独立队列，公平性由 WRR 权重表达。

## 3. WRR 调度：按请求个数服务

SSD 是**单一服务器**（带宽/延迟都是 SSD 全局资源）。WRR 决定「先服务哪个 Jetty 的请求」，
服务时间由 §4 给出。

- 一轮内，队列 i 依次服务 `weight_i` 个**请求**（不是字节量）。
- 一轮结束后轮到下一队列；某队列为空则跳过，进入下一队列。
- 语义：`weight` 越大，该 Jetty 每轮被服务的请求个数越多 → 得到的带宽份额越高
  （在请求大小近似相等时，≈ 按 weight 加权；请求大小不等时，严格按「个数」加权）。

伪代码：

```
scheduler:
  round = 0
  loop:
    q = round % N
    served = 0
    while served < weight[q] and not q.empty():
      req = q.dequeue()
      serve(req)                 # 占用 SSD 一个服务周期，见 §4
      served++
    round++
```

## 4. 服务时间模型

SSD 是单一服务器，每个请求（read 或 write）独占一个服务周期：

```
serve_time = (latency 一次性项) + bytes / ssd_bandwidth
```

- latency 是一次性固定开销（读 `ssd-read-latency`、写 `ssd-write-latency`）；
  `bytes / ssd_bandwidth` 是带宽限速项。
- 多个 Jetty 的请求在 SSD 上**串行化**：同一时刻只有一个请求在 SSD 上被服务，
  服务完当前请求才轮到 WRR 挑下一个。
- SSD 带宽上限即 `ssd_bandwidth × ssd_count`；WRR 只决定顺序，不增加并行度。

### 4.1 latency 按事务收一次，而不是按 TA segment 收

一个超过 `UB_WQE_TA_SEGMENT_BYTE`（64KB）的读/写 WQE 会被 TA 层切成多个 64KB segment，
每个 segment 作为一次独立的入站请求进入 ASU，并各自产生一段 response。SSD gate 挂在这些
segment response 的路径上，因此 `bytes/bandwidth` 天然按 segment 逐段计（总传输时间 =
总字节/带宽不变），但 **固定 latency 若逐 segment 收会被人为放大 N 倍**（N = segment 数）。

实现上，`UbTransaction` 用「同一个 Jetty 上最近一次过 SSD 的 WQE id」判断当前 segment 是否
开始了一个新的读/写事务：`chargeLatency = (lastTaskId[jetty] != 当前 taskId)`。只有新事务的
**第一个 segment** 收 latency，后续 segment 只走带宽项。因此一个 4MB 读（64×64KB）的 SSD
服务时间是 `50μs + 4MB/bandwidth`，而不是 `64×50μs + 4MB/bandwidth`。

> 前提：同一 Jetty 上连续读/写事务的 WQE id 互不相同（WQE id 由 lifecycle 分配、不重复），
> 且同一事务的多个 segment 在 TA 层按 SSN 顺序连续送达，故「taskId 变化」等价于「事务边界」。

## 5. 数据路径挂载点

### 5.1 读方向（ASU 出数据：NPU 读 KV）

现有链路：`NPU 发 URMA_READ → ASU UbTransaction 解析 → 建 WQE → 入 Jetty →
Jetty 产 segment → 经 TP 发送`（核心在 `UbTransaction::TriggerScheduleWqeSegment`
→ `RunTrackedPrepareRtpWqeSegment` → `RunTrackedAdmitRtpWqeSegment` → `WqeSegmentTriggerPortTransmit`）。

挂载方式：**在 segment 产出/回包前加一道 SSD gate**（实现挂在
`UbTransaction::EnqueueInboundTaResponse`，即每段 response 发送前）：

1. 请求（WQE）入 Jetty 后，其每个 segment 的 response 先在 SSD 队列排队。
2. SSD 的 WRR 选中该请求 → 按 §4.1 只对**新事务的第一个 segment** 收 `read_latency`，
   再按 `ssd_bandwidth` 的节奏逐 segment 放行（segment 产出速率被 SSD 带宽限住）。
3. 只有被 SSD 放行的 segment 才进入现有 `Prepare/Admit/Transmit` 流程，继续走 CBFC/DCQCN。

效果：ASU 出数据速率 = `min(ssd_bandwidth × ssd_count, 网络可发速率)`；
`ssd-count=1`（12GB/s < 200G）时 SSD 是读方向的真瓶颈，`ssd-count=5`（60GB/s）时瓶颈
回到网络侧。

### 5.2 写方向（ASU 收数据：NPU/1825 写 KV）—— 先做简化版

写方向 ASU 是**接收方**，SSD 写是「事后落盘」，只能靠反压间接限速（见 §8 与读方向
的区别）。分两档：

#### 5.2.1 简化版（本期实现）：延迟 ACK

- 收到一个写事务后，ASU 按 WRR 排队，等 `write_latency + bytes / ssd_bandwidth`
  「落盘」完成后才回写完成/ACK。
- 不做显式的 ingress drain / credit 停发建模：带宽限速通过**延迟 ACK** 间接传导
  （对端 transport 窗口受 ACK 节奏驱动而减速）。
- hook 点：`UbTransaction` 的入站写事务完成/确认路径，推迟 ACK 时机。

#### 5.2.2 保真版（后续再加）：ingress drain 服务器

- 把 SSD 写做成 ingress buffer 的「drain 服务器」：带宽限制抽走速率，drain 慢 →
  buffer 涨 → 触发 CBFC credit 停发/丢包。
- 与 ingress 准入（headroom/shared pool）耦合，实现成本高，作为后续迭代。

## 6. 实现步骤

1. **Step A（核心）**：加 `UbSsd`（或挂在 `UbFunction` 上）组件：
   - 按 Jetty 分队列（`jettyNum % ssd_queue_count`）。
   - WRR 调度器（按请求个数，权重可配）。
   - 服务时间 = `latency + bytes/bandwidth`。
   - 把 SSD gate 接进读方向 segment 产出路径。
2. **Step B（简化写方向）**：在入站写事务完成路径做「延迟 ACK」
   （`write_latency + bytes/bandwidth`），先不做 ingress drain。
3. **Step C（后续）**：保真版 ingress drain 服务器。

## 7. 验证方案

- 单 ASU 读 4MB：加 SSD 后，ASU 侧多 ~383μs（latency 50μs + 带宽项 333μs），
  且 ASU 聚合读吞吐被压到 ~12GB/s（对比无 SSD 时 ~200G）。
- 多 Jetty（多源并发）：验证 WRR 权重能改变各 Jetty 的带宽份额（按请求个数）。
- 对照：`ssd-bandwidth` 设成无限大 + latency=0 时，退化为当前行为（回归无损）。

## 8. 读方向 vs 写方向的本质区别

| | 读方向 | 写方向 |
|---|---|---|
| ASU 角色 | 生产者（出数据）| 接收者（收数据）|
| SSD 瓶颈作用方式 | 直接在 segment 产出处 gate 速率 | 只能靠反压（credit/ACK）间接限速 |
| hook 点 | 单一（segment 产出循环）| 分散（ingress 准入 / 重组 / 完成确认）|
| 本期实现 | 完整（Step A）| 简化「延迟 ACK」（Step B）|

写方向之所以麻烦：ASU 不直接控制发送速率，SSD 写带宽要先翻译成「接收速率上限」，
再通过 CBFC credit 停发或延迟 ACK 作用到对端；且落盘是 ingress buffer 的「drain」操作，
与 headroom/shared pool 耦合。保真版（§5.2.2）需要建模这条「接收 → 缓冲 → 落盘 → 反压」
闭环，故留到后续。
