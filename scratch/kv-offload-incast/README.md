# KV-offload Incast Case

Parameterized generator for the ASU -> union -> 1825 -> Ethernet-switch -> NPU topology that
models inference KV-cache offload read incast (NOF read: the NPU issues `URMA_READ`, every
ASU pushes its KV payload back across the UB|RoCE gateway).

## Usage

```bash
# 1:1 baseline (8 NPUs, balanced Ethernet-switch bandwidth)
python3 gen_case.py --output-dir scratch/kv-offload-incast \
    --asu-count 4 --npu-count 8 --union-count 2 \
    --asu-bw 200Gbps --fabric-bw 400Gbps --npu-bw 200Gbps --kv-bytes 4MB

# 8:1 incast stress (1 NPU, all ASU payloads converge on one NPU)
python3 gen_case.py --output-dir scratch/kv-offload-incast \
    --asu-count 4 --npu-count 1 --union-count 2 \
    --asu-bw 200Gbps --fabric-bw 400Gbps --npu-bw 200Gbps --kv-bytes 4MB
```

The ASUs each expose `union-count` ports (one 200G link to every union). Each union is a
**lossless bus** (fixed 100 ns forwarding delay + CBFC credit), not a store-and-forward switch: it
aggregates `asu-count` 200G ASU links into `gateway_port_count` 400G links toward its 1825 (1:1: for
the defaults, 4x200G = 2x400G), and each 1825 mirrors that capacity on its RoCE side into an Ethernet
switch (RoCE runs over Ethernet, so the RoCE side uses Ethernet PFC). Each NPU exposes a single
`--npu-bw` (200G) NIC into that switch. With `--npu-count 8` the switch bandwidth is 1:1
(8x200G out = 2x2x400G in); `--npu-count 1` oversubscribes it 8:1 for the incast study.
`*_bw` and `--kv-bytes` are fully parameterized; ASU/NPU counts scale through
`--asu-count`/`--npu-count`.

The `network_attribute.txt` sets `UB_VL_PER_NL "2"`, so CBFC transmit credit is pooled at NL
granularity: two VLs share one credit pool and are backpressured together. The 1825's receive
buffer (`SharedPoolBytes` + `HeadroomPerPortBytes`) is what grants that credit: it pulls WQEs
from the ASU only while the buffer has space, which is how the 1825 controls the ASU send rate.

## QoS (multi-VL priority isolation)

`--pri-high`/`--pri-low`/`--high-asu-count` split the ASUs into foreground (high-priority) and
background (low-priority) groups; each group's TP channels and read tasks carry its VL. The
strict-priority (SP) VL scheduler then serves the high VL first, so under incast the foreground
reads keep lower latency. Inspect with:

```bash
python3 analyze_qos.py scratch/kv-offload-incast
```

Root cause found and fixed: the incast drop is `ingress buffer full` in `UbQueueManager::
CheckIngressAdmission` — the per-port headroom (`HeadroomPerPortBytes`) was 64 KB, far below the
peer's CBFC initial credit (~1 MB = `CbfcInitCreditCell 6553` x cell 160 B), so during the
credit-return latency the headroom overflowed and dropped every burst. The generator now sets
`HeadroomPerPortBytes 2097152` (2 MB), which makes the 4-ASU 8:1 incast and the mixed-VL QoS case
both complete with 0 drops. QoS isolation is then observable (e.g. high 6352 us vs low 6846 us P99).
The drop is at the union/1825 ingress (UB side, CBFC headroom), not the RoCE switch egress — the
DCQCN mark->FECN->CNP->rate-limit loop is verified working end-to-end.

The generated files can be run with:

```bash
python3 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/kv-offload-incast'
```

## Gateway data-plane semantics (`--gateway-mode`)

`--gateway-mode` selects the 1825 data-plane semantics (`node.csv` `1825` type):

- `STORE_AND_FORWARD` (default): the 1825 buffers whole packets and switches them; the read is
  one end-to-end TP `ASU <-> NPU`.
- `HEADER_TRANSLATION`: the 1825 rewrites the header and forwards cut-through; still one end-to-end
  TP.
- `TRANSPORT_TERMINATION`: the 1825 becomes a transport endpoint (`UB_DEVICE`) and the read is
  split into two independent DCQCN domains:

  - UB side (`CBFC`): each 1825 reads every ASU over its own union (`gateway -> ASU URMA_READ`).
  - RoCE side (`PFC`): each NPU reads every 1825 shard (`NPU -> gateway URMA_READ`).

  The `transport_channel.csv` is split into `gateway <-> ASU` and `NPU <-> gateway` channels, and
  `routing_table.csv` gains union->gateway and RoCE-switch->gateway routes. The two reads are
  independent (not causally coupled): the gateway warms its cache on the UB side and serves the
  NPU reads on the RoCE side.

```bash
python3 gen_case.py --output-dir scratch/kv-offload-incast-tt \
    --asu-count 4 --npu-count 1 --union-count 2 --gateway-mode TRANSPORT_TERMINATION
```

## Inference workload (`--workload inference`)

Model per-layer inference as compute + KV cache I/O, using **GLM-4-9B** geometry as the concrete
example (`num_layers 40`, `hidden 4096`, `8 KV heads x head_dim 128`, `intermediate 13696`, bf16).
No attention/MLP is executed; compute is only a duration derived from a FLOPs estimate divided by
`--tflops`, and communication is `URMA_READ` / `URMA_WRITE` of the sharded KV cache.

Structure per NPU (each NPU runs one request, concurrent across NPUs if `--npu-count > 1`):

- **Prefill** (`--prompt-tokens P`): for each layer, `COMPUTE` (prefill FLOPs) then
  `URMA_WRITE` of `P` tokens of KV to the layer's ASU.
- **Decode** (`--decode-steps N`): for each step t and each layer, `URMA_READ` the layer KV
  (`P + t` tokens), `COMPUTE` (decode FLOPs), then `URMA_WRITE` the one new token's KV.

A **prefix KV-cache hit** is modeled with `--prefix-hit-tokens H` (0 = cold prefill): the first
`H` prompt tokens are assumed already resident in the ASU cache, so prefill instead does
`URMA_READ` of `H` tokens of KV, `COMPUTE` only the remaining `P-H` suffix tokens, then
`URMA_WRITE` of that suffix. This turns prefill into a read-heavy phase (hit) rather than a
write-heavy phase (cold).

KV is sharded by layer: layer `i` lives on ASU `i % asu_count`. Per-token per-layer KV is
`2 x num_key_value_heads x head_dim x bytes = 4 KB` for GLM-4-9B, so a `P=1024` decode read is
~4 MB per layer.

The whole sequence is a strict `dependOnPhases` chain, so within one request reads are serial;
a single request is therefore not an incast. The multi-ASU -> one-NPU incast only re-emerges when
several NPUs run requests concurrently against the same ASUs (`--npu-count > 1`).

```bash
# one request, prompt 1024 tokens, 4 decode steps, GLM-4-9B
python3 gen_case.py --output-dir scratch/kv-offload-inference \
    --asu-count 4 --npu-count 1 --union-count 2 \
    --workload inference --prompt-tokens 1024 --decode-steps 4 --tflops 500

# GLM-5.2 (MoE)
python3 gen_case.py --output-dir scratch/kv-offload-inference-glm52 \
    --asu-count 4 --npu-count 1 --union-count 2 \
    --workload inference --model glm-5.2 --prompt-tokens 1024 --decode-steps 4 --tflops 500
```

## Model config (models/*.json)

Model geometry is read from `models/<name>.json` via `--model <name>` (default `glm-4-9b`).
Required fields: `name`, `num_layers`, `hidden_size`, `num_attention_heads`,
`num_key_value_heads`, `head_dim`, `intermediate_size`, `bytes_per_param`.

Optional MoE fields (omit for a dense model):

- `num_experts`: total routed experts per layer.
- `num_experts_per_tok`: top-k experts activated per token.
- `shared_expert`: 1 if a DeepSeek-style shared expert is always active, else 0.

KV cache is attention-only (independent of MoE): `2 x num_key_value_heads x head_dim x bytes`
per token per layer. Compute is scaled by the active FFN count `num_experts_per_tok +
shared_expert` (a dense model defaults to 1). So MoE changes the compute FLOPs, not the KV size.

```json
{
  "name": "GLM-5.2",
  "num_layers": 78,
  "hidden_size": 6144,
  "num_attention_heads": 64,
  "num_key_value_heads": 64,
  "head_dim": 256,
  "intermediate_size": 12288,
  "bytes_per_param": 2,
  "num_experts": 256,
  "num_experts_per_tok": 8,
  "shared_expert": 1
}
```

## Protocol split

- UB side (ASU, union): `CBFC` (NL-granular credit) + DCQCN.
- RoCE side (NPU, RoCE switch): `PFC_DYNAMIC` + DCQCN.
- 1825: per-port flow control (UB ports `CBFC`, RoCE ports `PFC_DYNAMIC`), set through
  `rocePortCount` in `node.csv` and the per-node `flowControl` override.

## Verification

The default `--asu-count 4 --npu-count 1` config pushes KV on both of every ASU's ports (8 read
streams) that converge on one NPU across the two 1825 gateways. The run completes end-to-end;
UB-side ports emit `CbfcTrace`, RoCE-side ports emit `PfcDynamicTrace`/`PfcTrace`, and the NPU
port trace records the full incast ingress. Under the oversubscription at the RoCE switch the
PFC headroom is exceeded and packets drop (recovered by retransmission) — the congestion
behavior this case is meant to expose.

A previous `transport_channel.csv` tpn collision (every ASU sharing NPU tpn 0) made the run stall
indefinitely; the generator now assigns a unique NPU-side tpn per ASU/port.

The `scratch/kv-offload-minimal` case is the single-path baseline (ASU -> 1825 -> NPU).