# KV Cache SSD 卸载的 UB↔RoCE 异构互联架构
## 1. 背景与目标

推理场景下将 KV cache 卸载到后端 SSD，以节省 NPU HBM。后端存储为 ASU，
计算侧为 RoCE 网络的 NPU。两侧协议不同，需要网关桥接。本项目验证该异构
链路的 **QoS 性能**，核心研究对象是 **UB 与 RoCE 之间拥塞信息如何传递**。

协议细节不保真，只要求语义与性能对齐。

## 2. 拓扑（可参数化）

```
ASU×N (SSD + 1815e，每块 2×200G 口)
   │  │  (每个 ASU 两端口分别连两个 unions)
unions (UB 总线，与 1825 一一对应，固定 100ns 转发时延)
   │  │  (2×400G 口)
1825  (UB↔RoCE 网关)
   │
交换机 (RoCE 侧)
   │
NPU (RoCE 侧，HBM 为 KV 落点)
```

- ASU 数量、NPU 数量通过参数设置。
- 参数示例：4 ASU × 2 口 × 200Gbps → 2 unions → 每条 2×400Gbps → 2×1825 → 交换机 → 1 NPU。
- 每个 ASU 上的 1815e 有两个 port，分别连到两个 unions；unions 与 1825 为一对一关系。
- union 不是 store-and-forward 交换机，而是**无损总线**：固定 100ns 转发时延 + CBFC credit
  逐跳无损反压（模型上仍是 SWITCH 节点配 `forwardDelay=100ns` + CBFC，不加新节点类型）。

## 3. 协议映射（复用现有 UB 栈）

| 域 | L2 流控 | 拥塞控制 |
|----|---------|----------|
| UB 侧（ASU/unions/1825） | CBFC（credit，per-VL） | 方案2：CTP CC（端到端 CNP）；方案1：1825 RP 下发 rate |
| RoCE 侧（1825/交换机/NPU） | PFC | 同上（1825 是 RP 汇聚点） |

- 拥塞控制有两种方案（`--dpu-role` 切换）：
  - **方案2（端到端 CTP CC）**：FECN 标记 → NPU 发 CNP → ASU 降速，长链路反馈。
  - **方案1（1825 RP）**：1825 收 PFC/CNP → 按 ASU 流量比例分配 rate → 发带 rate 的 CNP
    下发 ASU 控速，近距离反馈，收敛更快（实测快 3.6~3.9 倍，见 findings §17）。
- RoCE 侧不实现真实 RoCE 协议，复用 UB 节点仅替换 L2 为 PFC。
- 跨域差异集中在 L2 无损反压：UB 侧 credit 停发 ⟷ RoCE 侧 PFC pause。

## 4. 节点角色

| 节点 | 角色 | 网络侧 | L2 流控 | 拥塞控制 |
|------|------|--------|---------|----------|
| ASU（1815e） | SSD 存储 + UB-only NIC | UB | CBFC（per-VL credit） | 方案2：CTP CC / 方案1：接收 1825 rate |
| unions | UB 总线，收敛多 ASU（100ns 转发时延） | UB | CBFC | 不参与 |
| 1825 | UB↔RoCE 网卡（直通 + QP） | 双侧 | UB 侧 CBFC（per-VL credit）、RoCE 侧 PFC | 方案1 的 RP（收 PFC/CNP 分配 rate） |
| 交换机 | RoCE 侧转发 | RoCE | PFC | 打 FECN（方案2） |
| NPU | 计算侧，HBM 为 KV 落点 | RoCE | PFC | 方案2：发 CNP |

## 5. 数据流（NOF 读语义）

- NPU 发送一个小的 `send` 命令给 ASU，其中 `opcode=read`，告诉 ASU 如何处理请求。
- ASU 收到命令后，主动把 KV cache 数据写进 NPU 的 HBM。
- 方向：小请求 NPU→ASU，大数据 ASU→NPU。对应"发起方 NPU / 目标方 ASU"的读事务
  （request 小、response 大）。
- **incast 方向**：多个 ASU 同时向同一个 NPU 推 KV 数据（多→1 进 NPU），
  拥塞点落在 1825 的 RoCE 出口与 NPU 侧交换机。

## 5.1 UB 侧流控模型

UB 侧无损与速率控制是两个层次，流控点都在**发送端点**（不是 union）：

| 方向 | 数据发送方 | 无损反压（谁授 credit） | 速率控制（谁做 DCQCN） |
|------|-----------|------------------------|------------------------|
| 读（ASU→1825→NPU） | 1815e（ASU） | 1825 授 credit | **1815e** |
| 写（NPU→1825→ASU） | 1825 | （1825 收信用 credit） | **1825** |

- **1825 的 buffer 控制 1815e 发送速度**：1825 有一个接收 buffer（
  `SharedPoolBytes` + `HeadroomPerPortBytes`），buffer 有空位才授 credit 给 1815e，
  1815e 拿到 credit 才拉 WQE 发数据；buffer 满就停授 credit，1815e 停发。这就是
  CBFC 的 credit 环，credit 量由 `CbfcInitCreditCell` 决定，buffer 大小决定能兜住
  多大的 ASU 瞬发 credit。
- DCQCN 只是端到端的速率微调（FECN/ECN→CNP→降速），无损本身靠 CBFC credit 保证。
- DCQCN 的 LineRate/InitialRate **按发送端口定速**（`UB_DCQCN_USE_PORT_RATE=true` 时从
  topology.csv 的端口线速推导）：1815e/NPU=200G、1825 RoCE=400G，不再用一个全局 fabric 值。

## 6. 1825：设计探讨的三种语义 vs 实际落地的网卡直通 + QP

> 迁移到最新 ns-3-ub 后，1825 实际落地为**网卡直通 + QP**（见 §6.5），不再用
> 早期 `UbGateway1825` 的三语义网关实现。下面 6.1–6.4 是早期的三种语义探讨（背景），
> 保留作参考；6.5 是当前实现。

三种语义用属性（enum）切换，核心差异在于"两层拥塞信息如何跨过网关"。

### 6.1 拥塞信息的两个层次

1. **链路级无损反压**（逐跳）：UB 用 credit（CBFC），RoCE 用 PFC pause。
   共同货币是网关出口 buffer 的满度。
2. **端到端拥塞通知**（速率信号）：两侧均为 DCQCN 的 FECN/ECN 标记 + CNP 反馈。

### 6.2 存储转发（StoreAndForward）

- 收完整一个 UB 报文进 buffer，解析后重新封装成 RoCE 侧报文再发。
- 链路级反压：靠 buffer 满度隐式传递（UB 侧不回 credit，RoCE 侧触发 PFC）。
- 端到端信号：两侧各自按本段 queue 独立打 FECN/ECN，不跨网关闭环。
- 代价：整体延迟高、buffer 深。

### 6.3 头翻译（HeaderTranslation / Cut-through）

- 只改写包头（UB LPH/NTH → 以太网+RoCE 头），payload 边到边转发。
- 链路级反压：credit↔PFC 逐事件显式映射。
- 端到端信号：FECN↔ECN 逐字段直通。
- 代价：延迟最低，但字段无 1:1 映射，语义对齐成本最高。

### 6.4 传输终结（TransportTermination / Proxy）

- 1825 是两侧完整协议端点：UB 侧终结 TP 连接，RoCE 侧终结 QP，数据被完整拷贝。
- 链路级反压：完全隔断，网关同时可见两侧状态，主动映射 credit↔PFC。
- 端到端信号：两侧各自构成独立 DCQCN 域，网关在两端各做源/目的，
  内部做 QoS 映射与限速。
- 代价：状态最重、延迟最高、内存最大，但 QoS 语义最完整。

### 6.5 当前实现：网卡直通 + RoCE 侧 QP

迁移后 1825 落地为**网卡**（`UbDpu`），不再继承交换机的 store-and-forward 语义：

- **UB 侧（对 ASU）**：CBFC，per-VL credit（每 VL 独立授 credit）。
- **RoCE 侧（对交换机/NPU）**：QP 形式——每条 `{源 ASU, 目的 NPU, VL}` 一个 QP（即
  `DpuFlowKey`），数据按 flow 入对应 QP 队列再发 RoCE 侧。
- **转发**：UB↔RoCE **直通**（`DirectForwardCtpDataPacket`），跳过 ingress buffer 记账、
  VOQ、调度；转发即释放 UB 侧 credit。
- 早期 `UbDpu : public UbSwitch` 继承了交换机的 buffer/VOQ/调度，导致 victim 流在 DPU
  内部堆积饿死（findings §14–15）。改成直通后解决。

## 7. QoS 模型

- 1825 内部存在多个 QP，映射到若干 VL；每个 port 有 16 个 VL。
- 提供 QP→VL 映射表；当前阶段所有 QP 映射到同一 VL（单一优先级），
  后续再扩展多优先级隔离。
- 验证场景：incast 下不同优先级的流量隔离与延迟保障。

## 8. 建模边界

- RoCE 侧不建模真实 RoCE 报文：复用 UB 节点，仅替换 L2 流控为 PFC、
  拥塞控制为 DCQCN，实现语义对齐而非协议保真。
- KV cache 访问模式与规模由推理模型参数自动生成，ASU/NPU 数量参数化。
- 1825 的三语义、16 VL、QP→VL 映射均为参数化建模对象。

## 9. 术语约定

- 复用 `CONTEXT.md` 中的 UB 术语（Entity、VL、TP channel、CBFC、CAQM、DCQCN、
  PFC、credit、FECN/ECN、CNP 等）。
- "RoCE 侧 / UB 侧"表示拓扑域，不改变其中节点的 TB 协议栈语义。

## 10. 排障与参数结论

incast 卡死、headroom 定容、PFC/DCQCN 分工、尾丢与重传的完整排查与参数讨论，
见 `kv-cache-offload-ub-roce-findings.md`。