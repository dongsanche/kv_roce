# KV Cache SSD 卸载 UB↔RoCE 网关 — 实施计划

## 0. 目标回顾

验证推理 KV cache 卸载场景下 UB↔RoCE 异构链路的 QoS 性能，核心是
**UB 与 RoCE 之间拥塞信息的传递**。协议不保真，只求语义与性能对齐。
详见 `kv-cache-offload-ub-roce.md`。

## 1. 已锁定事实

- 拓扑（参数化）：ASU(1815e) → unions(UB 交换机) → 1825 网关 → RoCE 交换机 → NPU。
- 协议：UB 侧 CBFC+DCQCN；RoCE 侧 PFC+DCQCN；两侧 CC 统一为 DCQCN。
- 1825：三语义（存储转发/头翻译/传输终结）参数切换；每 port 16 VL；QP→VL 映射表。
- Q 数据流：NOF 读，NPU 发 `opcode=read` 小命令，ASU 主动将 KV 写进 NPU HBM；
  incast 为多 ASU→单 NPU。
- QoS 阶段：当前全同优先级、单 VL，先跑通 incast 与三语义对比。

## 2. 默认项（无异议按此执行）

| 项 | 默认 |
|----|------|
| 网关节点类型命名 | node.csv 中写 `1825` |
| RoCE 侧实现 | 复用 UB 节点，仅替换 L2=PFC、CC=DCQCN |
| 第一阶段 QoS | 单优先级、单 VL |
| KV 负载 | 先支持单一读块大小（由模型参数导出），复杂分页后续加 |
| 度量指标 | P99 读延迟、1825 buffer 深度、CNP/PFC 触发计数、吞吐 |

## 3. 阶段计划

### Phase 0 — 前置准备

- [ ] `git submodule update --init --recursive`（初始化 `ns-3-ub-tools`）。
- [ ] 首次 configure + 构建 `ub-quick-example`：
  `python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples
  --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja`，
  然后 `python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example`。
- [ ] 用 `--PrintTypeIds` / `--PrintUbGlobals` 初始化运行时参数目录，
  记录 VL/流控/DCQCN 当前默认值。

### Phase 1 — 1825 网关模型（C++，核心）

- [ ] 新增 `UbGateway1825` 节点类，注册 TypeId。
- [ ] 三语义 enum attribute：`StoreAndForward` / `HeaderTranslation` /
  `TransportTermination`。
- [ ] 每 port 16 VL + QP→VL 映射表（当前全映射同一 VL）。
- [ ] 数据面语义：
  - 存储转发：UB 侧 credit 满停发；RoCE 出口 buffer 满触发 PFC；DCQCN 按本段 queue 打点。
  - 头翻译：线速头改写，credit↔PFC 逐事件映射，FECN↔ECN 直通。
  - 传输终结：两侧独立 DCQCN 域，1825 做两端 QP 端点，内部 QoS 映射 + 限速。
- [ ] L2 桥接核心：UB 侧 credit 状态 ↔ RoCE 侧 PFC pause 映射。

### Phase 2 — 节点类型接入

- [ ] `node.csv` 支持 `1825` 节点类型（`ub-utils.cc` CreateNode 分支）。
- [ ] 1825 的 UB 口配 `CBFC+DCQCN`，RoCE 口配 `PFC+DCQCN`。
- [ ] RoCE 侧节点复用 UB 节点，只换 L2/CC。

### Phase 3 — 单测

- [ ] 复用 `ub-node-test-fixture.h`，三语义各一组固定小拓扑单测：
  - 存储转发：延迟 = 整包缓存 + 转发。
  - 头翻译：credit↔PFC 事件映射、FECN↔ECN 直通。
  - 传输终结：两侧 DCQCN 独立闭环、QP 端点行为。
  - 16 VL 结构 + QP→VL 映射正确性。
- [ ] 运行 `ninja -C cmake-cache unified-bus-test` 或按模块测试验证。

### Phase 4 — 端到端最小 case（incast）

- [ ] 拓扑：4 ASU / 2 unions / 2 1825 / 1 NPU（全部参数化）。
- [ ] 负载生成器：NOF 读，NPU 发 `opcode=read`，ASU 回推 KV 大块进 NPU HBM；
  KV 块大小由模型参数（层数/头数/头维/seq len/精度）导出，ASU/NPU 数量参数化。
- [ ] 输出证据：P99 读延迟、1825 buffer 深度、CNP/PFC 触发计数、吞吐。

### Phase 5 — QoS 深化

- [ ] 多 QP → 多 VL 的优先级隔离。
- [ ] incast 下验证高优先级流保延迟。

## 4. 证据计划（默认）

| 指标 | 定义 |
|------|------|
| P99 读延迟 | 从 NPU 发 send(read) 到 KV 落 HBM 完成，分 QP/VL 统计 |
| 1825 buffer 深度 | 每 port/每 VL 的峰值与平均占用 |
| CNP/PFC 触发计数 | 每方向 DCQCN CNP 与 PFC pause 次数 |
| 吞吐 | ASU→NPU 有效数据吞吐 vs 端口线速 |

## 5. 验证方式

- 模型层：Phase 3 单测（`unified-bus` test-runner）。
- 集成层：Phase 4 端到端 case 跑通并输出上述证据。
- 对照：三语义在相同拓扑/负载下横向对比，记录拥塞信息传递差异。