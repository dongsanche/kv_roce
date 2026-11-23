# KV Cache 卸载 UB↔RoCE 网关 — 排障与参数结论

本文沉淀 4-ASU → 2-union → 2×1825 → RoCE-switch → NPU 拓扑在 **8:1 超额认购
（1 个 NPU）下卡死**的根因排查与参数讨论。原始架构见 `kv-cache-offload-ub-roce.md`，
实施计划见 `kv-cache-offload-ub-roce-implementation-plan.md`。

## 1. 结论速览

| 问题 | 结论 |
|------|------|
| 8:1 incast 卡死根因 | UB 侧（union/1825）入口 headroom 默认 64KB，兜不住 CBFC 初始 credit（~1MB）的突发 |
| 修复 | `HeadroomPerPortBytes` 64KB → **2MB**（2×初始 credit） |
| DCQCN | 端到端是通的（mark→FECN→CNP→降速），不是卡点 |
| PFC | 监控 ingress 不是 egress，不需要"看见 egress" |
| 丢包位置 | union/1825 的 ingress 准入（CBFC 侧），**不是** RoCE switch 的 egress |
| 重传为何不恢复 | 尾丢不触发 NAK 快传；超时重传又撞满 buffer 再丢 |

## 2. 更正过的关键事实（排查中得出的正确结论）

### 2.1 DCQCN 反馈环是通的

实测（打开 `UB_CONGESTION_CONTROL_TRACE_ENABLE` + 探针）：

1. RoCE switch 打 FECN：627 次（`DcqcnMarkTrace`）。
2. FECN 标记**确实传到** NPU（709 个标记包，`mode=4 fecn=1`）。
3. NPU 生成 CNP（`hasSentCnp` 0→1）。
4. CNP 到达 ASU（28 次 `CNP_RX`），`OnSenderCongestionNotification` 执行降速。

> 早期"FECN 标记丢了 / 不发 CNP"是探针加错分支（只打未标记包）导致的误判，已更正。

### 2.2 PFC 监控 ingress，不监控 egress

PFC（IEEE 802.1Qbb）是**接收端口按优先级监控自己的 ingress（RX）缓冲**，
越 XOFF 就发 pause。它跟 egress 输出队列没有直接关系。ns3-ub 自己的
`src/unified-bus/doc/switch-buffer-architecture.md` 也写明：InPort 视图管 PFC，
OutPort 视图（VOQ+egress）管 DCQCN/ECN。

> 所以"PFC 需要看见 egress 吗"——不需要。正确的表述是"ingress 缓冲有没有正确反映拥塞"。

### 2.3 丢包点在 UB 侧 ingress，不是 RoCE egress

探针统计丢包位置（`RecordRuntimePacketDrop("ingress buffer full")`）：

| 位置 | 次数 |
|------|------|
| union（node 4） | 411 |
| union（node 5） | 253 |
| 1825（node 6） | 573 |
| 1825（node 7） | 595 |
| RoCE switch（node 8） | **0** |

丢包原因是 **"ingress buffer full"（入口准入），不是 "egress queue buffer full"**。
congestion 下沉到 RoCE switch(8:1) 反压不回来，数据在 union/1825 的 ingress
共享池里堆，入口准入丢包。

## 3. 根因：headroom 过小

### 3.1 三层缓冲归属（`ub-queue-manager.cc`）

| 缓冲 | 参数 | 归属 | 当前值 |
|------|------|------|--------|
| reserve | `ReservePerQueueBytes` | 每个 (port, VL) 队列保底 | 0 |
| shared | `SharedPoolBytes` | **全交换机共享** | 12MB |
| headroom | `HeadroomPerPortBytes` | **每个 ingress port 独立**（port 内所有 VL 共享） | 原 64KB → 2MB |

headroom 的 Occupancy 是 `m_hdrmBytes[inPort][priority]` 按 (端口, 优先级) 记账，
`GetIngressPortOccupancy(inPort)` 把同一 inPort 的所有 VL 相加得到 `portHdrmUsed`，
判断 `portHdrmUsed + pSize > m_headroomPerPortBytes` 即丢——限值按 port 卡，非全局。

丢包条件（`CheckIngressAdmission`）：

```
headroom 满（port 内 VL 合计 > HeadroomPerPortBytes）且 shared > xoff → 丢弃
```

### 3.2 为什么 64KB 会卡、2MB 就好

headroom 要兜的是"发端在反压生效前还能注入的量"。UB 侧是 **CBFC credit 制**：

- 发送端被授予 `CbfcInitCreditCell 6553 × 160B ≈ 1MB` 初始 credit；
- 在"别再发了"（不再还 credit）生效前，发送端可以**合法地再灌 ~1MB**；
- 64KB headroom 远小于这 ~1MB 突发 → 直接丢包。

实测（4-ASU 8:1，4MB/流）：

| Headroom | 结果 |
|----------|------|
| 64KB（旧默认） | ❌ 卡死 |
| 512KB | ✅ 完成（557 丢包）|
| **2MB** | ✅ **0 丢包** |

修复：`scratch/kv-offload-incast/gen_case.py` 的 `HeadroomPerPortBytes "2097152"`。

## 4. 为什么"尾丢"不靠重传恢复（三层因果链）

### 4.1 尾丢不触发 NAK 快传

NAK 快传的触发条件：接收端**先收到后面的包（PSN=N+1），发现中间 N 缺号**，才发 NAK。

"尾丢"丢的是**最新到达的包**（入口准入放不进去），接收端拿到连续前缀 `0..K`，
只是在等 `K+1`——`K+1` 后面没有包来"揭示这个洞"，所以**没有乱序缺口，不发 NAK**。
实测 `NAK_RCVD`（ASU 收到 NAK）计数 = **0**。

### 4.2 超时重传（RTO）又逃不出去

RTO 会触发：发送端等不到 `K+1` 的 ACK，超时后重发 `K+1`。但重发的包又被同一个
仍然满着的 buffer 再丢一次，形成：

```
尾丢 → RTO 超时 → 重传 → 又被丢 → RTO（翻倍）→ 重传 → 又丢 → ...（最多 MaxRetransAttempts=7 后放弃）
```

根子还是 buffer 一直满：DCQCN 降速和 CBFC credit 反压都有**反馈延迟**，这段空窗里
发送端仍在灌，headroom 太小兜不住。

### 4.3 headroom 要兜的是 credit，不是 BDP

| 机制 | 发端在反压生效前能灌多少 | headroom 兜的量 |
|------|--------------------------|-----------------|
| PFC（RoCE，商用交换机） | 只受 BDP 限制（无 credit，pause 一到就停） | ≈ **BDP**（几十~几百 KB） |
| CBFC（UB 侧） | 手里有 ~1MB 初始 credit | ≈ **初始 credit（~1MB）** |

- BDP 50KB = 400G × 1μs 的"线上在飞"数据量。
- CBFC 发端一开始就拿 1MB credit，可合法再灌 1MB，headroom 要兜的是这个。
- 所以"BDP 50KB 却要 2MB headroom"不矛盾：BDP 管 PFC/线缆在飞，credit 管 CBFC 发端的发送额度。

## 5. 参数讨论小结

| 参数 | 讨论结论 |
|------|----------|
| headroom | 每 ingress port 独立（非全局）；UB 侧按 CBFC 初始 credit（~1MB）配，不是 BDP；2MB 合理 |
| egress 队列 | 真实交换机是几十~几百 KB（不是"几个 MTU"，也不是 512KB 的锅）；调小不解决卡死 |
| SharedPoolBytes | 全交换机共享的大池子（12MB） |
| PFC vs DCQCN | PFC 盯 ingress，DCQCN/ECN 盯 egress，分工天然如此 |
| 模块实现 | union = `UbSwitch`(CBFC)；1825 = `UbGateway1825`(CBFC+PFC)；缓冲账务共用 `UbQueueManager` |

## 6. 走过的弯路（避免再踩）

1. ~~PFC 看不见 egress → bug~~：错。PFC 本就不看 egress，方向整个错。
2. ~~FECN 标记丢了 / 不发 CNP~~：错。探针加错分支，DCQCN 环是通的。
3. ~~egress 队列太大（512KB）是根因~~：错。丢包是 ingress buffer full，不是 egress queue full；调小 egress 不解决。
4. ~~CBFC credit 链卡死~~：错。credit 在正常流动（CONTROL_SEND / CREDIT_RESTORE 各 544 次）。

真正的根因是第 3 节的 **headroom 过小**。

## 7. 结果（headroom 修复后）

- 8:1 incast（4 ASU → 1 NPU，4MB/流）：0 丢包跑通。
- 混合 VL QoS（VL1 高 / VL7 低）：0 丢包，隔离可观测：

| 优先级 | P50 | P99 |
|--------|-----|-----|
| 高（VL 1） | 6346 μs | **6352 μs** |
| 低（VL 7） | 6515 μs | **6846 μs** |

- 单优先级全扫（priority 1–8，4MB/流）：全部 0 丢包跑通。之前观察到的
  "priority 3/4 卡死"是同一个 headroom 问题的症状（旧 64KB 下所有优先级都丢包，
  3/4 的特定时序导致重传未收敛成硬卡），不是独立 bug。

## 8. 架构对齐：总线 union + 1825 流控 + DCQCN 按端口定速

从前几节不丢包后，进一步按真实架构对齐了三处建模语义（详见
`kv-cache-offload-ub-roce.md` 第 5.1 节）。关键结论：

### 8.1 union 是总线，不是 store-and-forward 交换机

- union 建模为**无损总线**：固定 `forwardDelay=100ns` + CBFC（credit）。
  不需要新节点类型，`SWITCH` 节点配 `forwardDelay=100ns` + CBFC 即可。
- 曾尝试把 union 设成 `FlowControl=NONE`（误读"总线=无流控"），结果在 8:1 incast 下
  首轮突发即打穿 headroom 丢包；改回 CBFC 才无损。**总线 = 低时延(100ns) + 无损(CBFC)**，
  不是无流控。

### 8.2 1825 的 buffer 控制 1815e 发送速度（credit + pull WQE）

- 1825 有一个接收 buffer（`SharedPoolBytes` + `HeadroomPerPortBytes`），buffer 有空位才授
  CBFC credit 给 1815e，1815e 拿 credit 才拉 WQE 发数据；buffer 满就停授 credit，1815e 停发。
- credit 量 = `CbfcInitCreditCell`(6553 cell ≈ 1MB)，headroom 2MB（2× 初始 credit）兜住瞬发
  来势；已验证 0 丢包。
- 流控点在**发送端点**：读方向由 1815e（DCQCN + CBFC credit），写方向由 1825。DCQCN 只是速率
  微调（FECN/CNP→降速），无损本身靠 CBFC credit 保证。

### 8.3 DCQCN 按发送端口定速（不再用全局 fabric 值）

- 原配置 `UbHostDcqcn::LineRate/InitialRate = {fabric_bw}`(=400G) 是错的：1815e/NPU 实为 200G NIC。
- 新增 GlobalValue `UB_DCQCN_USE_PORT_RATE`（默认 `false`，向后兼容），`UbHostDcqcn::OnTpAttached`
  在开启时从**发送端口的线速**（topology.csv 里 `SetDataRate` 的值）推导 LineRate/InitialRate：
  1815e/NPU=200G、1825 RoCE=400G。
- KV-cache 用例打开该开关（`global UB_DCQCN_USE_PORT_RATE "true"`）；实测 ASU 的 DCQCN 起始速率
  从 400G 变成 200G，unified-bus 单测保持 PASS（默认关闭，不影响既有测试）。

### 8.4 三态拥塞传导结论（对照）

同一 incast 下三种 1825 语义的实操差异（8MB × 8 流 → 1 NPU）：

| 语义 | PFC pause | 丢包 | 拥塞怎么跨 1825 |
|------|-----------|------|----------------|
| 存储转发 | 1903 | 0 | buffer 满度反压穿透（union/ASU credit 掉到 0）|
| 头翻译(cut-through) | 6 | 8941 | 不反压、靠丢包+重传 |
| 传输终结 | 397 | 0 | 不跨（两侧独立 DCQCN 域）|

即「存储转发=反压穿透 / 头翻译=丢包穿透 / 传输终结=隔离」，可用 credit 掉深 + PFC 次数 + 丢包数
三个量分开测。
## 9. CTP 下的 TT 中继缺口（待修）

切换到 CTP 传输 + TRANSPORT_TERMINATION 后，发现 1825 目前**不是耦合中继**：

- traffic.csv 里 UB 侧读（`1825→ASU`）和 RoCE 侧读（`NPU→1825`）都是 `phase=0`、无
  `dependOnPhases` 依赖 → 两侧是**两个独立 URMA_READ**。
- 结果：1815e 的控速（CBFC credit + CTP CNP）只压住了 UB 侧（ASU→1825 这段），
  RoCE 侧（1825→NPU）从 1825 已有的 buffer 直接回数据、**不等 UB 侧那一读**，照旧线速灌
  8 条流到 NPU → RoCE switch 出口打爆 → PFC 兜不住 → 丢包。

**修法**：给 `NPU→1825` 的 RoCE 侧读加 `dependOnPhases`，依赖 `1825→ASU` 的 UB 侧读，让
1825 变成「先读 ASU、再回 NPU」的真中继，1815e 的控速才会沿 credit 链传导到 RoCE 侧。

## 10. CTP 下 TT 模式「route cannot be found」根因与修复

TT + CTP 初次跑通后，RoCE 侧仍有 **54 个丢包**，`First drop reason: route cannot be found`，
全部集中在 **RoCE switch（node 8）**，且丢的是 **CNP（拥塞通知）包**，不是数据包。

### 10.1 根因链

1. NPU 收到 FECN 标记的读响应数据（1825→NPU）后，按 `m_cnpInterval` 发 CNP 回 1825 降速。
   此 CNP 由 `UbCtpTransportService::SendCnp` 构建。
2. `SendCnp` 只写 `NodeIdToCna16(dstNode)`（**通配目的端，无端口**），且不写 `SetLb`
   （lb 恒为 0）。
3. CNP 到达 RoCE switch，走 `ResolveCtpOutput` 的 entity 解析分支：
   `SelectCtpEntityMemberPort(dstEntity, lb=0) = ports[0 % 4]`。
   TT 模式下 1825 是 UB_DEVICE，默认 entity 1 的 member ports 是**全部 4 个口**（0/1 UB 侧 +
   2/3 RoCE 侧）；`ports[0]` 是 **UB 侧 port 0**。
4. RoCE switch 的 `routing_table.csv` 只配了 `dstNode=1825, dstPortId=2/3`（RoCE 侧），
   **没有 port 0 的路由** → `route cannot be found` 丢包（每侧 27 次，共 54）。

### 10.2 修复

CNP 应回发到**被标记数据包的源端口**（即 1825 的 RoCE 端口 2/3），而非按 entity 通配均衡
（均衡可能选中 UB 侧端口）。改动很小：

- `SendCnp` 增加 `dstPortId` 入参，`SetDcna` 由 `NodeIdToCna16(dstNode)` 改为
  `NodeIdToCna16(dstNode, dstPortId)`（`dstPortId == UINT32_MAX` 时保持通配）。
- 调用点（FECN 处理处）从 `cnaHeader.GetScna()` 解出被标记数据包的源端口，传给 `SendCnp`。

修复后 TT+CTP / SF+CTP / TT+RTP 三个组合均 `Simulation finished`、**0 丢包**，
unified-bus 单测全绿。

> 教训：TT 模式 1825 的默认 CTP entity 横跨 UB/RoCE 两侧端口，后面任何"按 entity 通配
> 解析目的端口 + 默认 lb=0"的控制包回发（如 CNP），都会命中 `ports[0]`（UB 侧）而走到
> 没有路由的一侧。真正的中继应该让 1825 的两侧各自暴露独立 entity，而不是共用一个
> 全端口 entity。

## 11. CNP 修复后的无损性能（吞吐与突发扫描）

CNP 修复后（§10），4-ASU / 2-union / 2×1825 / 1-NPU（8:1 汇聚）TT+CTP 场景 0 丢包、
无损，吞吐贴着 200G 出口线速走。

### 11.1 读语义（先对齐模型）

`URMA_READ` = 读事务：请求小、响应大。NPU 发小读命令，ASU 作为**数据发送方**主动把
数据写回（`READ` → 生成 `READ_RESPONSE`，ub-transaction.cc）。实测不对称度：NPU 出口只发
~361KB（8 个读请求），回收 33.78MB 数据，约 1:93。TT 模式下这是"两段代理读"：
`NPU→1825`（RoCE）+ `1825→ASU`（UB），数据 ASU→网关→NPU。

### 11.2 吞吐随突发增大逼近线速

单 200G NPU 出口，8 条读竞争（`--transport-mode CTP --ctp-rate-ai 20Gbps`）：

| kv/读 | 总数据 | makespan | 有效吞吐 | vs 200G |
|-------|-------|----------|---------|---------|
| 4MB | 32MB | 1422 µs | 188.8 Gbps | 94.4% |
| 8MB | 64MB | 2807 µs | 191.2 Gbps | 95.6% |
| 16MB | 128MB | 5530 µs | 194.2 Gbps | 97.1% |
| 32MB | 256MB | 10929 µs | 196.5 Gbps | 98.2% |
| 64MB | 512MB | 21753 µs | 197.4 Gbps | 98.7% |

- **0 丢包**一直到 512MB 总量，重传未触发。
- 固定开销（读建立、CC 爬升）被大负载摊薄 → 越接近线速。
- UB 侧 CBFC credit 掉到 ~850KB（初始 ~1MB）就稳住、不到零：**限速由速率差决定，不由
  突发总量决定**，突发变大只拉长 makespan。

## 12. 拥塞控制分层：DCQCN 只在 RoCE 侧激活，UB 侧只有 credit 窗口

开 `UB_CONGESTION_CONTROL_TRACE_ENABLE`（4MB 场景）：

| 位置 | FECN 标记 | CNP |
|------|----------|-----|
| RoCE switch → NPU（8:1 瓶颈） | **3732 次** | NPU→网关 75 个 |
| union → 网关（UB 侧） | **1 次** | 网关→ASU ~1 个 |

- **RoCE 侧才是 DCQCN 主战场**：200G 出口打满 → switch 打 3732 次 FECN → NPU 发 75 个
  CNP → 网关降速。
- **UB 侧没有拥塞可标**：网关 16MB buffer 把 ASU 突发整个吃掉，union→网关队列几乎从没
  超过 Kmin，DCQCN 不启动。
- 所以 UB 侧看到的 credit 降到 ~850KB **不是"拥塞反压"**，是 credit 窗口的正常在途限制
  （ASU 想发 400G，网关只能 ~100G 吐）。TT 模式 proxy 的 buffer 把两个拥塞域"完全隔断"
  （§6.4），UB 域无拥塞、RoCE 域才是 DCQCN+PFC 叠加。

## 13. 受害流（HOL 阻塞）验证：多 incast 场景

### 13.1 credit 真实粒度是 NL，比 VL 更粗

`ub-cbfc.h`：CBFC credit 按 **NL（网络通道）** 记账，`UB_VL_PER_NL=2` 即 2 个 VL 共用一个
信用池，"exhausting that pool blocks every VL in the group together"。受害流的成立条件：
**同一信用池（或同一 priority）里的两条流 → 去不同目的地 → 只有一个拥塞**。单 NPU 拓扑
（所有流同汇一个口）不满足，所以 §11 里看不到受害流。

### 13.2 多 incast 场景（`--workload multi-incast`）

给 `gen_case.py` 新增 `--workload multi-incast`：NPU0 读 `ASU1..N`（incast），其余每个 NPU
只读 `ASU0`（专属无辜源）。用 `--asu-bw 100Gbps --fabric-bw 400Gbps` 把网关逼成**单 RoCE
口**，让所有流共享同一条 400G 链路，ECMP 不打散。

场景：4 ASU(100G) → union → 网关(400G 单口) → switch → NPU0/NPU1(各 200G)。
NPU0 读 ASU1/2/3（incast 300G→200G），NPU1 读 ASU0（专属源、无辜）。`--pri-high 1
--pri-low 1` 让四流同 priority。

### 13.3 结果与三层分解（如何分辨受害 vs 带宽竞争）

分辨方法：**baseline 定参考 + 量链路利用率判是否真打满 + 换 priority 做 A/B 拆层**。

| 场景 | NPU1 读 4MB 完成时间 | 增量 |
|------|--------------------|------|
| 受害流单独（baseline） | 338.6 µs | — |
| +incast，受害流换 pri7 | 436.2 µs | +97.6 µs |
| +incast，受害流同 pri1 | 504.5 µs | +165.9 µs |

链路利用率实测：共享 400G 链路只跑 **248 Gbps（62%，有空闲）**，受害专属源 ASU0 被压到
**67 Gbps（低于自身 100G）** → 不是"公平带宽竞争"（那要链路打满、源不亏），是纯反压。

三层分解：

- **+97.6 µs（pri7 那段）**：CBFC 反压穿过网关 buffer——网关 RoCE 侧被 incast 堵 → buffer
  涨 → UB 侧停授 credit → 专属源 ASU0 也被压慢。**不看 priority**（与网关 buffer 共享有关）。
- **+68.3 µs（pri1 比 pri7 多）**：RoCE 侧 **PFC 按 priority 的 HOL**。

结论：单 NPU 拓扑没受害流，多 incast 拓扑有；粗粒度（per-NL credit + per-priority PFC）
反压确实伤及无辜流，priority 隔离只能减轻（+68µs 可被消掉）、不能根除（+97µs 的网关
buffer 穿孔反压仍在）。

### 13.4 新增产物

- `scratch/kv-offload-incast/gen_case.py`：新增 `--workload multi-incast` 分支
  （`targets = asu_ids[1:] if npu_idx == 0 else [asu_ids[0]]`）。
- 复现命令：
  `python3 scratch/kv-offload-incast/gen_case.py --output-dir <CASE> --asu-count 4 --npu-count 2
  --union-count 1 --asu-bw 100Gbps --fabric-bw 400Gbps --gateway-mode STORE_AND_FORWARD
  --transport-mode CTP --ctp-rate-ai 20Gbps --kv-bytes 4MB --workload multi-incast --pri-high 1 --pri-low 1`

---

> 以下第 14–16 节是迁移到最新 open-usim `ns-3-ub` 后新增的内容。迁移时移除了
> 旧实现的污染项：`UbGateway1825`（双流控网关）、`relay`/`CQE`/`per-NL(UB_VL_PER_NL)`、
> `union-marking-scope`、`tt_mode`、`UB_DCQCN_USE_PORT_RATE` 等，相关旧结论（§5 的
> `UbGateway1825`、§13 的 `UB_VL_PER_NL`、§8.3 的 `UB_DCQCN_USE_PORT_RATE`）不再适用。

## 14. victim 流饿死：多优先级下低优先 VL 被 SP + PFC 全局阈值双重饿死

### 14.1 现象

`--workload multi-incast --pri-high 1 --pri-low 7`（NPU0 高优先读 ASU0/1/2，NPU1 低优先读 ASU3）
下，低优先 victim（VL7）的 response 收不到，读不完成、request 反复重传。早期现象被
误判成多个根因（详见 §14.5），最终用日志逐跳定位才钉死。

### 14.2 真实根因（数据钉死）

1. **DPU 出口 SP 调度饿死 VL7**：victim 的 544 个 response 到 DPU6 后，`VOQ[port3][VL7]`
   大部分时间空（源头供给慢），DWRR 没得转。早期误判"DWRR 失衡"——实际 `SEL/SEND`
   日志显示 VL1/VL7 完全公平交替，deficit 机制正确。
2. **源头 ASU3 只发 544 个就停**：Jetty inflight 窗口 `m_inflightMax=512`（`ub-function.h`），
   发了 544 个、只有 34 个到 NPU10、ACK 只回 34 个 → 未 ACK 510 ≈ 512 → 窗口满 → ASU3 停发。
3. 恶性循环：response 堆 DPU → 到 NPU 少 → ACK 少 → 窗口满 → 源头停发 → 供给断。

### 14.3 根子的根子：UbDpu 错用交换机语义

迁移初期 `UbDpu : public UbSwitch`，继承了交换机的
`ingress buffer 记账 + VOQ + DWRR 调度 + CBFC credit 反压`。DPU 是网卡，本该
「UB 口收一个、RoCE 口发一个」直通，结果被套上交换机才有的 buffer 排队，victim 的
response 在 DPU 内部堆积出不去。

## 15. 修复：DPU 网卡直通 + RoCE 侧 QP（per-flow）

### 15.1 直通转发

`UbSwitch` 加 `DirectForwardCtpDataPacket` 钩子，`UbDpu` override：UB→RoCE 的数据
直接 `EnqueueToEgress` + `TriggerTransmit`，**跳过 ingress buffer 记账、VOQ、DWRR 调度**；
转发即 `OnIngressReleased` 释放 UB 侧 CBFC credit。

### 15.2 RoCE 侧 QP = per-flow 队列

`DpuFlowKey{源 ASU, 目的 NPU, VL}` 即一个 QP/TP，`DpuFlowState` 挂 `pending` 队列。
UB 侧收数据按 flow 入对应 QP 队列再发 RoCE 侧；RoCE→UB 方向（request/CNP）直接透传。
UB 侧仍按 VL 管理 CBFC credit（per-VL），RoCE 侧按 flow（QP）。

### 15.3 效果

victim 从卡死（137KB）→ 完整 16.9MB，16/16 任务、0 丢包；RTP / CTP 同优先级 /
RP 多优先级回归全过。

## 16. 另一个真实 bug：node.csv 的 flowControl 列没被解析

### 16.1 现象

方案1（1825 RP）跑多优先级时 588 丢包（`ingress buffer full`），incast 只发 8.85MB
（应 48MB）。排查发现 RoCE8 的 PFC pause 从没触发（`PFC-EVAL` 全 `pause=false`）。

### 16.2 根因

`gen_case.py` 在 `node.csv` 里写了 `flowControl` 列（RoCE8 = `PFC_DYNAMIC`），但
`ub-utils.cc` 的节点解析**只读到 `rocePortCount`，没读 `flowControl`**。于是 RoCE8 一直
是默认 CBFC（不是 PFC），PFC 反压从不生效，incast 满速灌爆 CBFC 的 ingress headroom 丢包。

### 16.3 修复

`NodeEle` / `UbStructuredNodeConfig` 增加 `flowControl` 字段，`CreateNode` 里
`SetAttribute("FlowControl", ...)`。修复后方案1 立即 0 丢包、16/16。

## 17. 方案1 vs 方案2 性能（PFC 反馈是关键）

两种拥塞方案（详见架构文档 §3）：

- **方案2（CTP CC 端到端）**：FECN 标记 → NPU 发 CNP → ASU 降速（长链路反馈）。
- **方案1（1825 RP）**：1825 收 PFC/CNP → 按 ASU 流量比例分配 rate → 发带 rate 的 CNP 下发 ASU。

`--dpu-role rp --rp-push --rp-pfc-feedback`（`RpPfcFeedbackEnabled`）切换是否用 PFC 感知。
实测（multi-incast，pri-low 7，DWRR）：

| 方案 | victim 均值 | incast 均值 |
|------|------------|------------|
| 方案2（CTP CC） | 1594 µs | 5096 µs |
| 方案1 不用 PFC（只靠 CNP） | 1594 µs | 5096 µs |
| 方案1 用 PFC | **439 µs** | **1296 µs** |

- 方案1 **去掉 PFC 后性能与方案2 完全一致**（都是端到端 CNP 反馈）。
- 方案1 快 3.6~3.9 倍的全部来源是 **PFC 近距离反馈**（RoCE8 一跳 pause 到 1825，就地分配
  下发 ASU），不是「集中式分配算法」本身。

即 1825 的价值在「离拥塞点近、能用链路层 PFC 立即感知」，而非「它算 rate 算得多聪明」。

## 18. 方案1 重做：CNP 死代码 → per-flow 重构 → CQE 携带 → 直通反压 bug

本节记录方案1（1825 RP）在迁移后的一次较大重做，以及重做中暴露出的一个数据面反压 bug。
时间顺序：先发现 CNP 观测是死代码，再把分配从「目的 NPU 池」改成 per-flow，再落地
「CQE 携带 rate」，最后在 4MB/8:1 压测时发现直通路径的反压 bug。

### 18.1 CNP 观测是死代码（"方案1 只靠 CNP"从没生效过）

**现象**：`--dpu-role rp --rp-pfc-feedback=false`（只靠 CNP）下，DPU 的 rate 从不变化，
结果与方案2（端到端 CTP CC）完全一致。

**根因**：`UbDpu::DirectForwardCtpDataPacket` 把所有 CNA16 包（包括 NPU→ASU 方向的 CNP）
都直接透传（UB↔RoCE 直通），`ExamineCtpDataPacket` 里的 `CTP_CNP → OnCnpSeen` 分支
**永远走不到**。所以 §17 里「方案1 不用 PFC = 方案2」的真正原因不是「CNP 稀疏」，
而是 DPU 压根收不到 CNP。

**修复**：把 CNP 观测挪进 `DirectForwardCtpDataPacket` 的 RoCE→UB 透传分支。

### 18.2 分配粒度重构：从「目的 NPU 池」到 per-flow AIMD

**问题**：原实现里 `DpuDestState`（`m_destStates`）按 **目的 NPU 一个带宽池** 记账，
`OnCnpSeen` 把 flow 级 CNP 映射成「整个 NPU 池砍半」。一条重流的 CNP 会连带把同 NPU
下的受害流（别的 VL/源）一起压没——与「1825 做 per-flow 控速」的设计目的矛盾。

**修复**：删除 `DpuDestState` / `RpAllocAlgo`（PROPORTIONAL/MAXMIN/WEIGHTED 三选一）。
每个 `DpuFlowState`（`{源 ASU, 目的 NPU, VL}`）独立维护 `currentRate + alpha + congested`，
在 `RecomputeAllocation`（55µs）里逐 flow 做 AIMD：

- 该 flow 看到 CNP → 乘性减（`rate *= 1 - alpha/2`）；
- 无 CNP → 加性增（`rate += RpRateAi`）；
- clamp 到 RoCE 线速。

CNP 只标记它自己那条 flow，受害流不再被连坐。

### 18.3 CQE 携带 rate（RP 不透传 CNP）

**目标**：CNP 稀疏（`CcCnpInterval` 50µs 下 4MB 一个读只 ~3 个 CNP），改用「读完成 CQE
（小 IO 下频率高）携带 rate」，不依赖 PFC。

改动：

- `CtpOpcode` 新增 `CTP_CQE = 0x2`（2-bit 字段装得下）。
- `UbCtpTransportService::SendCompletion`：NPU 读完成（`UbApp::OnTaskCompletedVisible`，
  `CqeEnabled` 打开）发一个带 `UbCnpExtTph(rate=0)` 的 CQE 回 ASU。
- DPU 在 RoCE→UB 看到 `CTP_CQE`，把该 flow 的 `currentRate` 写进 CQE rate 字段再发回
  ASU（`PushRateToAsu(..., CTP_CQE)`），原始 CQE 吞掉。
- ASU 收到 `CTP_CQE` 解析 rate → `OnExternalRate` 限速。
- **RP 模式不透传 CNP**（吞掉），避免 ASU 自己的 DCQCN 和 RP 双重降速。
- 兜底：`RpCqeFallbackEnabled` 下，对超过 `RpCqeFallbackInterval`（默认 250µs）没收到
  CQE 的 flow 主动补推一次 rate，防止流卡死时收不到 rate 更新。

**1MB 实测**（4 ASU → 1 NPU，CTP）：

| 模式 | incast 重流完成时间 |
|------|--------------------|
| transparent（方案2） | 1670 µs |
| rppush（方案1 + PFC） | 169 µs |
| cqe（方案1 + CQE 携带） | 169 µs |

cqe 与 rppush 打平、比 transparent 快 ~10×；之前 CNP 双重惩罚导致的过限消失。

### 18.4 DPU 直通反压 bug（4MB / 8:1 丢包，待修）

**现象**：同样 4 ASU / 2 union / 2×1825 / 1 NPU（8:1 汇聚）下，`--kv-bytes 4MB` 有
**29096 个 `ingress buffer full` 丢包**，任务卡死、exit 超时；1MB 则 0 丢包。

**根因（数据钉死）**：`UbDpu::DirectForwardCtpDataPacket`（UB→RoCE 方向）现在的顺序是：

1. 一收到 ASU 的包，先 `OnIngressReleased` **无条件把 CBFC credit 还给上游**；
2. 然后才 `flow.pending.push` + `TrySendQp` 转发到 RoCE egress。

当 RoCE egress 被下游 PFC 反压住（egress 队列 `MaxEgressBytes`=512KB 满），
`EnqueueToEgress` 失败丢包，但 credit 已经还回去 → 上游 ASU 继续灌 → union/1825 的
ingress buffer（12MB shared + 2MB headroom）被打爆 → `ingress buffer full` 丢包。

即**不是水线/缓冲不够，而是直通路径把反压链切断了**：credit 应该「转发成功才还」，
现在变成了「收到就还」。PFC/CBFC 水线再低也救不了，因为 DPU 这个网卡在数据面直接丢，
没把自己的下游拥塞传导到上游。

**待修**：把 credit 归还（`OnIngressReleased`）挪到「真正被 RoCE egress 收下之后」：
`DirectForward` 收到先 `pending.push` 不还 credit；`TrySendQp` 检查
`UbEgressQueue::CanEnqueue`，有空间才 enqueue + 还 credit + pop，没空间留在 `pending`；
并补一个 egress 排空后的重试钩子，否则 pending 里的包没人发。

## 19. union per-NL「误打标」受控实验：假设被证伪，风暴真因是自打标

背景：union（CBFC 总线交换机）按 NL(2 VL) 聚合打 ECN（`VlPerNl=2`）。此前担心
「同 NL 内低优 VL 积压会连坐高优 VL」（[NL-MARK] 强制打标时确实 VL6/VL7 一起被标）。
为干净验证，做了两件事：

1. **union-only 阈值**（`UB_UNION_DCQCN_KMIN/KMAX/PMAX` 三个 GlobalValue，默认与
   `UbSwitchDcqcn` 一致 5120/204800/0.01）：只改 union 打标灵敏度，RoCE 交换机保持默认，
   gen_case 用 `--union-kmin/--union-kmax/--union-pmax` 下发。
2. **DcqcnMarkTrace 增强**：新增 `vl`（被标包所在 VL）和 `vl_backlog_bytes`
   （NL 内 per-VL 积压，如 `vl6=4124;vl7=0`）两列，直接归因每次 mark。

### 19.1 敏感化风暴复现（RoCE 侧零参与）

s3 + 8MB + `--union-kmin 1024 --union-kmax 8192 --union-pmax 0.1`：union 打标 760 次
（p 最高 1.0），**RoCE 交换机（node8）无任何打标 trace**，每 ASU 收 330–420 个翻译后
cut-CNP → 速率乘性崩到亚 1G → makespan 148ms + 1056 丢包。即之前全局调阈值看到的风暴，
单靠 union 打标即可完整复现。默认值回归：与旧基线逐字节一致（0 丢包）。

### 19.2 同 NL vs 隔离 NL 受控对比：连坐不成立

处理组 `--pri-high 6 --pri-low 7`（VL6/VL7 同处 NL3）vs 对照组 `--pri-high 4`
（VL4 独占 NL2，VL7 独占 NL3），其余全同：

| 指标 | 同 NL (6,7) | 隔离 NL (4 / 7) |
|---|---|---|
| 高优 VL 被标次数 | 355 (vl6) | 376 (vl4) |
| 低优 VL7 被标次数 | 388 | 381 |
| 被标时 own backlog min/avg/max | 4124/4124/4124 | 4124/4178/8248 |
| 被标时 sibling backlog 均值 | ~11B (vl6)；0 (vl7) | 0；0 |
| own≤1400B（≈空）的 mark 数 | **0** | **0** |
| 每 ASU CNP 数 | 328–408（各自 VL） | 328–400（各自 VL） |
| 任务完成时间 | 114–140µs | 114–143µs |

**结论：两组统计不可区分，跨 VL 连坐为 0。** 每次 mark 时被标 VL 自己的 VOQ 就是
~4124B，兄弟 VL 几乎总是空的（NL 聚合 ≈ own）。机理：SP 调度 + CBFC credit 阻塞轮转
下，union 出口 VOQ 是「单 VL 独占式」建排（一个 VL 攒到 ~4124B 打标/降速/排空，换下一个），
两个 VL 的 VOQ 极少同时非空，所以 per-NL(2) 打标 ≡ per-VL 打标。

之前 [NL-MARK] 的「VL6 跟着 VL7 被标」只在强制打标（人为拉满积压）下出现，
自然 dynamics 不触发。

### 19.3 推论：不存在「只打拥塞 VL」的 Kmin 窗口

所有 VL 共享同一 ~4124B 操作点（credit 周期的吸引子），而默认 Kmin=5120 恰好高于它
（union 从不打标）。一旦把 Kmin 降到 4124 以下让 union 开始打标，所有 VL 会一起自打标
（风暴）；不存在只让「拥塞 VL」被打、无辜 VL 幸免的窗口。要按 VL 区分 congestion，
需 per-VL 阈值/占有率归一化，而不是 NL 聚合或全局 Kmin。

（次要观察：同 NL 组 vl7 own 最大 4124，隔离组可达 24744——mark 随机性导致轨迹分岔，
不影响归因结论。）

### 19.4 修正：19.2 的"连坐不成立"结论存在设计缺陷，受害流实验补测

19.2 的对照实验有个自我削弱的设计问题：两条"无辜流"本身都是 8MB 对称大流，自己也会把
VOQ 堆到 ~4124B > Kmin=1024 **自打标**——实验测的是"饱和自打标之上 NL 共享额外多打几枪"
（=0），而**不是**"自己绝不拥塞的流是否会被同 NL 兄弟积压打中"。误打标的定义要求受害流
own < Kmin <= 聚合，该实验里根本不存在这种流。

补测（受害流实验）：ASU0 = 10 个 4KB 小读、间隔 16µs（own 积压 ~1-2 包），ASU1-3 =
6 个 8MB 大流；Kmin=3072 卡在受害流 own 积压（<3KB）与大流钉点（4124B）之间，
Pmax=1.0（先测"能不能发生"）。同 NL（受害 VL6）vs 隔离 NL（受害 VL4）两臂。

**结果：两臂受害 VL 都是 0 mark**（大流各 2553 mark，own 恒 4124，sibling 恒 0）。
Pmax=1.0 下受害包经过时聚合 = 4124+1408 > Kmin → p≈0.48，若真与大流同队应得 ~13 个
mark，实测 0 —— 不是没设计好，是**受害包根本没在队列非空期穿过**。

### 19.5 机理收口：积压停在源头，union 队列 97% 时间是空的

1µs 粒度 QueueTrace 采样（拥塞窗口 40-250µs，union node4 出口 port4）：
**3145 个采样只有 89 个非零（2.8%），峰值恰好 4124B**。即：

- 所谓"VOQ 钉在 4124"不是常驻队列，而是 **CBFC 信用批次的瞬态幅度**：一批信用到账、
  突发 ~4124B 一次入队、µs 级排空，然后队列回到 0。之前所有 mark 采样到的 4124 只是
  "突发经过时的幅度"，不是持续水位。
- 反压链的实际形态：RoCE 侧 8:1 汇聚 → PFC/信用一路回传 → **积压被停在源头**
  （ASU 的 CBFC 信用窗口 + 被 CNP 砍崩的 CTP CC 发送窗口），而不是停在 union 队列里。
  union 出口 VOQ、union ingress、DPU UB 侧 ingress 三处采样全部接近空
  （node4 ingress max 44B；DPU node6 UB ingress avg 195B）。
- 因此受害流的包几乎总在"空窗期"穿过 union 出口：聚合 = own 1-2 包 < Kmin → 不打标。
  要误打标成立，需要受害包**恰好与同 NL 大流的突发同队**（µs 级窗口，2.8% 占用率），
  轻流量流撞上的概率近零。

### 19.6 中间结论（后被 19.8 推翻，保留作过程记录）

1. ~~NL 聚合打标在本拓扑/本负载下无害~~——当时的三组实验（对称大流对照、10 包受害流、
   DWRR 对照、1000 包受害流）全部 0 false-mark。
2. 方法论教训（仍然有效）：19.2 的对照实验在两臂都自打标饱和时测增量，天然检测不到
   误打标；测"误伤"必须让受害流的 own 积压低于 Kmin（Kmin 卡窗口式设计），
   并把 Pmax 拉到 1 排除随机性，再按 `vl_backlog_bytes` 列归因到每一个 mark。

### 19.7 为什么五组实验都是 0：CC 反馈环把驻留堆积压没了

1000 包受害流实验（19.5）期望 ~13 个 false-mark，实测 0——不是概率而是结构性错过。
1µs QueueTrace 采样显示 union 出口 VOQ 97% 时间为空，而这是 **CTP CC 开启**的直接后果：

    大流自打标 -> cut-CNP 风暴 -> 大流源头速率崩到 ~1G
    -> 注入速率 << 链路速率 -> union 出口每 ms 只有 ~2-5 个 credit 批次
    -> VOQ 只在批次瞬间 ~4124B、us 级排空 -> 受害包(1/ms)撞不上 0.1% 占空窗口

即"union 出口无驻留"是 CC 反馈环的**平衡态**，不是架构不变量。

### 19.8 连坐证实：PFC 主导驻留时 per-NL 打标 100% 误伤受害流

反压的正确打开方式（也是真实 RoCE incast 的形态）：**源头不响应 CNP、PFC/credit 反压
主导**。实验：`--no-cc` 关掉 ASU 源头 CTP CC，但 `UB_CC_ENABLED=true` 保留 union ECN
打标（network_attribute 手工改），大流持续全速灌。

受害流：1000 个 1408B 读、间隔 1us（own 积压 ~1.4KB）；大流：6x8MB（VL7）；
Kmin=3072（> own 1.4KB、在大流驻留之下）、Pmax=1.0。同 NL（受害 VL6）vs 隔离 NL（VL4）。

| 指标 | 同 NL (受害=vl6) | 隔离 NL (受害=vl4) |
|---|---|---|
| union node4 出口 VOQ 驻留 | port4: 37.2% 时间非空、均值 650KB、峰值 1.0MB | （同型） |
| 受害 VL false-mark（sibling>own） | **621，占受害包 62%** | **0** |
| 受害 own / sibling backlog | 1436B / 820KB avg、1.0MB max | own 1.4KB < Kmin，永不触发 |
| 大流 vl7 marks | 5358（全部 own-driven） | 5360（同型，控制干净） |

**因果链（与用户描述完全一致）**：

    vl7 大流在 RoCE 侧 8:1 汇聚被 PFC 反压
    -> DPU 转发不成功、不还 vl7 的 CBFC credit（per-VL credit）
    -> union 出口 vl7 VOQ 驻留堆积（MB 级，37-93% 时间非空）
    -> 受害 vl6（RoCE 侧畅通、credit 正常归还）的包持续穿过 union 出口
    -> NL 聚合 = own 1.4KB + vl7 820KB >> Kmin -> 被打标
    -> 62% 的受害包被 FECN（Pmax=1 敏感化下）
    -> DPU 翻译成 cut-CNP 发回 ASU，无辜流被降速（CcEnabled=true 时）

隔离 NL 臂里同一个受害流 0 mark，证明误打标完全来自 NL 共享，且与两条臂大流自身
的 5358/5360 marks 同型对照干净。

### 19.9 最终结论（替代 19.6）

1. **per-NL(2) 打标的跨 VL 连坐是真实存在的机制**，触发条件 = 同 NL 内一条 VL 的
   RoCE 侧路径拥塞（credit 断供驻留堆积）+ 另一条 VL 畅通。受害流自身拥塞为零
   （own 1.4KB << Kmin）时仍被 62% 误打。
2. 之前"无影响"的表象是 **CTP CC 开启时 CNP 反馈环把源头砍崩、驻留堆积被压没** 的
   平衡态——它是 CC 过度限速的副产品，不能当作 per-NL 无害的证据。真实 RoCE 里
   PFC 反压驻留是 incast 常态，19.8 的形态比 19.5 更接近现实。
3. 设计含义：union 的 NL 聚合粒度在"兄弟 VL 反压驻留"时会把畅通 VL 一起打标。
   若要保护高优/受害流，需要 per-VL own-backlog 归因打标（`GetOutPortVlBacklogBytes`
   已具备该能力），或把不同业务类分到不同 NL。
4. 观测工具：`DcqcnMarkTrace` 的 `vl_backlog_bytes` 列可以直接量化 false-mark
   （sibling>own）比例，是这套实验得以成立的关键。
