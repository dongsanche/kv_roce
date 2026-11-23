#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate an OpenUSim KV-cache offload incast case.

Topology (parameterized):

    ASU (UB, CBFC) -> union (UB switch) -> 1825 (UB|RoCE gateway) -> RoCE switch -> NPU (PFC)

ASUs are split evenly across `union_count` unions, each union fans into one 1825 gateway,
and all gateways fan into one RoCE switch that converges on `npu_count` NPUs. The workload
is the NOF read pattern: each NPU issues URMA_READ against every ASU, so all KV payloads
flow ASU -> NPU and converge on the NPU (incast).

With `--gateway-mode TRANSPORT_TERMINATION` the 1825 becomes a transport endpoint and the
read is split into two independent DCQCN domains: gateway -> ASU on the UB/CBFC side, and
NPU -> gateway on the RoCE/PFC side.

With `--workload inference` the traffic is a per-layer GLM-4-9B prefill + decode sequence:
each layer emits COMPUTE (duration derived from per-layer FLOPs / --tflops) plus URMA_READ /
URMA_WRITE against the sharded KV cache. KV is sharded by layer across the ASUs.
`--prefix-hit-tokens H` models a prefix KV-cache hit: prefill reads the cached `H` tokens
instead of recomputing them, and only computes/writes the `P-H` suffix.

Usage:
    python3 gen_case.py --output-dir scratch/kv-offload-incast \
        --asu-count 4 --npu-count 1 --union-count 2 \
        --asu-bw 200Gbps --fabric-bw 400Gbps --kv-bytes 4MB
"""
import argparse
import csv
import json
import os
import sys

DEFAULT_NETWORK_ATTRIBUTES = """default ns3::UbApp::EnableMultiPath "false"
default ns3::UbApp::RoutingType "PER_FLOW_SHORTEST_PATHS"
default ns3::UbLink::Delay "+0ns"
default ns3::UbPort::UbDataRate "{fabric_bw}"
default ns3::UbPort::UbInterframeGap "+0ns"
default ns3::UbPort::CbfcFlitLenByte "20"
default ns3::UbPort::CbfcFlitsPerCell "8"
default ns3::UbPort::CbfcRetCellGrainDataPacket "4"
default ns3::UbPort::CbfcRetCellGrainControlPacket "1"
default ns3::UbPort::CbfcInitCreditCell "6553"
default ns3::UbSwitch::FlowControl "CBFC"
default ns3::UbSwitch::VlScheduler "{vl_scheduler}"
# UB side keeps CBFC credit (lossless); the DPU grants credit to the ASU through the
# bus union, and its headroom absorbs the transient ASU initial-credit burst.
default ns3::UbDpu::UbFlowControl "CBFC"
default ns3::UbDpu::RoceFlowControl "PFC_DYNAMIC"
# DPU CC role: TRANSPARENT (pure forwarder, rate control lives on the ASU CTP CC) or RP
# (the DPU is the RoCE reaction point, paces its RoCE-side egress and backpressures the ASU).
default ns3::UbDpu::CcRole "{dpu_role}"
default ns3::UbDpu::GradedBackpressure "{graded_bp_enabled}"
default ns3::UbDpu::RpPushEnabled "{rp_push_enabled}"
default ns3::UbDpu::RpCqeFallbackEnabled "{rp_cqe_fallback_enabled}"
default ns3::UbTransportChannel::EnableRetrans "true"
default ns3::UbTransportChannel::BaseRTO "+25600ns"
default ns3::UbTransportChannel::MaxRetransAttempts "7"
default ns3::UbTransportChannel::RetransExponentFactor "1"
default ns3::UbTransportChannel::DefaultMaxWqeSegNum "1000"
default ns3::UbTransportChannel::DefaultMaxInflightPacketSize "1000"
default ns3::UbTransportChannel::TpOooThreshold "2048"
default ns3::UbTransportChannel::RoutingType "PER_FLOW_SHORTEST_PATHS"
default ns3::UbSwitchAllocator::AllocationTime "+10ns"
default ns3::UbQueueManager::ReservePerQueueBytes "0"
default ns3::UbQueueManager::SharedPoolBytes "{shared_pool_bytes}"
# SharedPoolBytes + HeadroomPerPortBytes are the 1825/union receive buffer. The 1825
# grants CBFC credit to the ASUs from this buffer space (pull WQE when space is free), so
# the buffer size directly bounds how much transient ASU credit can be in flight. The
# peer's initial credit is CbfcInitCreditCell(6553) x cell(160B) ~= 1 MB, so a per-port
# headroom below ~1 MB drops ("ingress buffer full") under incast. 2 MB (2x the initial
# credit) leaves margin.
default ns3::UbQueueManager::HeadroomPerPortBytes "2097152"
default ns3::UbQueueManager::AlphaShift "7"
default ns3::UbQueueManager::DynamicPfcResumeGapBytes "8192"
default ns3::UbHostDcqcn::CnpInterval "+50000ns"
# DCQCN LineRate/InitialRate are NOT set here: UbHostDcqcn::OnTpAttached derives them from the
# sender port's per-link data rate (topology.csv), so 1815e/NPU pace at 200G and the 1825 at 400G.
default ns3::UbHostDcqcn::RateIncreaseTimer "+55000ns"
default ns3::UbHostDcqcn::AlphaUpdateInterval "+55000ns"
default ns3::UbHostDcqcn::ByteCounterThreshold "10485760"
default ns3::UbHostDcqcn::RateAi "40Mbps"
default ns3::UbHostDcqcn::HyperAiRate "100Mbps"
default ns3::UbHostDcqcn::FastRecoveryLimit "5"
default ns3::UbHostDcqcn::AlphaGain "0.00390625"
default ns3::UbSwitchDcqcn::KminBytes "5120"
default ns3::UbSwitchDcqcn::KmaxBytes "204800"
default ns3::UbSwitchDcqcn::Pmax "0.01"

# UB transport: RTP (default) or CTP (compact, coarse per-dest-Entity/VL CNP rate control)
default ns3::UbApp::TransportMode "{transport_mode}"
default ns3::UbApp::CqeEnabled "{cqe_enabled}"
default ns3::UbCtpTransportService::CcEnabled "{ctp_cc_enabled}"
default ns3::UbCtpTransportService::CcLineRate "400Gbps"
default ns3::UbCtpTransportService::CcInitialRate "400Gbps"
default ns3::UbCtpTransportService::CcRateAi "{ctp_rate_ai}"
default ns3::UbCtpTransportService::CcHyperAiRate "100Mbps"
default ns3::UbCtpTransportService::CcRateIncreaseTimer "+55000ns"
default ns3::UbCtpTransportService::CcByteCounterThreshold "10485760"
default ns3::UbCtpTransportService::CcFastRecoveryLimit "5"
default ns3::UbCtpTransportService::CcAlphaGain "0.00390625"
default ns3::UbCtpTransportService::CcCnpInterval "{ctp_cnp_interval}"

# ASU SSD 存储模型：按 Jetty 分队列 + WRR + 延迟/带宽（PCIe 5.0 例值）
default ns3::UbSsd::ReadLatency "{ssd_read_latency}"
default ns3::UbSsd::WriteLatency "{ssd_write_latency}"
default ns3::UbSsd::Bandwidth "{ssd_bandwidth}"
default ns3::UbSsd::QueueCount "{ssd_queue_count}"

global UB_FAULT_ENABLE "false"
global UB_PRIORITY_NUM "16"
global UB_VL_NUM "16"
global UB_CC_ALGO "DCQCN"
global UB_CC_ENABLED "{ub_cc_enabled}"
# Per-node CTP CC opt-out: listed nodes' senders ignore CNPs/external rates (CC-blind).
global UB_CC_DISABLED_NODES "{cc_disabled_nodes}"
# Union-only DCQCN thresholds: applied only to CBFC bus switches (the union on the UB side);
# RoCE (PFC) switches keep the ns3::UbSwitchDcqcn defaults above.
global UB_UNION_DCQCN_KMIN_BYTES "{union_kmin}"
global UB_UNION_DCQCN_KMAX_BYTES "{union_kmax}"
global UB_UNION_DCQCN_PMAX "{union_pmax}"

global UB_TRACE_ENABLE "true"
global UB_TASK_TRACE_ENABLE "true"
global UB_PACKET_TRACE_ENABLE "false"
global UB_PORT_TRACE_ENABLE "true"
global UB_RECORD_PKT_TRACE "false"
global UB_FLOW_CONTROL_TRACE_ENABLE "true"
global UB_CONGESTION_CONTROL_TRACE_ENABLE "{cc_trace_enable}"

global UB_PARSE_TRACE_ENABLE "true"
global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"
global UB_QUEUE_SAMPLE_INTERVAL_NS "0"
"""


def parse_bytes(value: str) -> int:
    value = value.strip()
    units = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3}
    for suffix, mult in sorted(units.items(), key=lambda kv: len(kv[0]), reverse=True):
        if value.upper().endswith(suffix):
            return int(float(value[: -len(suffix)]) * mult)
    return int(value)


def parse_bw_gbps(value: str) -> int:
    value = value.strip().upper()
    if value.endswith("GBPS"):
        return int(float(value[:-4]))
    if value.endswith("MBPS"):
        return 0
    sys.exit(f"unsupported bandwidth {value!r}; use e.g. 200Gbps")


def scale_bandwidth(bw_str: str, count: int) -> str:
    """按块数放大 SSD 带宽（每块 bw_str，共 count 块），返回聚合带宽字符串。"""
    num_str = bw_str.rstrip("/s").rstrip("/S").strip()
    total_gb = parse_bytes(num_str) * count / (1024**3)
    return f"{total_gb:g}GB/s"


# GLM-4-9B (ChatGLM) public model geometry used as the fallback inference example.
# Compute is only ever a value derived from these sizes; no algorithm is executed.
GLM_4_9B = {
    "num_layers": 40,
    "hidden_size": 4096,
    "num_attention_heads": 32,
    "num_key_value_heads": 8,  # GQA
    "head_dim": 128,
    "intermediate_size": 13696,
    "bytes_per_param": 2,  # bf16
}

MODEL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")


def load_model_config(model_name):
    """Load a model geometry config from models/<name>.json."""
    if model_name.endswith(".json"):
        path = model_name if os.path.isabs(model_name) else os.path.join(MODEL_DIR, model_name)
    else:
        path = os.path.join(MODEL_DIR, model_name + ".json")
    if not os.path.exists(path):
        sys.exit(f"model config not found: {path}")
    with open(path) as f:
        return json.load(f)


def kv_bytes_per_token(model):
    """K + V for one token of one layer, in bytes."""
    return 2 * model["num_key_value_heads"] * model["head_dim"] * model["bytes_per_param"]


def ffn_active_multiplier(model):
    """Active FFN multiplier: 1 for a dense model, (top-k + shared expert) for MoE."""
    return model.get("num_experts_per_tok", 1) + model.get("shared_expert", 0)


def prefill_flops_per_layer(model, prefix_tokens, new_tokens):
    """Prefill FLOPs when `prefix_tokens` hit the cache and only `new_tokens` are computed.

    The new tokens' queries attend over the full `prefix + new` context, but only the
    new tokens' QKV/out/MLP projections are charged (the prefix K/V already exist).
    """
    h = model["hidden_size"]
    g = model["num_key_value_heads"]
    d = model["head_dim"]
    heads = model["num_attention_heads"]
    inter = model["intermediate_size"]
    m = ffn_active_multiplier(model)
    total = prefix_tokens + new_tokens
    qkv = 2 * new_tokens * h * (h + 2 * g * d)
    attn = 4 * new_tokens * total * heads * d
    out = 2 * new_tokens * h * h
    mlp = m * 6 * new_tokens * h * inter
    return qkv + attn + out + mlp


def decode_flops_per_layer(model, context_tokens):
    h = model["hidden_size"]
    g = model["num_key_value_heads"]
    d = model["head_dim"]
    heads = model["num_attention_heads"]
    inter = model["intermediate_size"]
    m = ffn_active_multiplier(model)
    qkv = 2 * h * (h + 2 * g * d)
    attn = 4 * context_tokens * heads * d
    out = 2 * h * h
    mlp = m * 6 * h * inter
    return qkv + attn + out + mlp


def flops_to_ns(flops, tflops):
    return int(flops / (tflops * 1e3))


def gen_inference_traffic(npu_ids, asu_ids, prompt_tokens, decode_steps, tflops, priority,
                          prefix_hit_tokens=0, model=None):
    """Emit per-layer prefill + decode as a strict phase chain per NPU.

    Each NPU runs one request (concurrent across NPUs). KV is sharded by layer:
    layer i lives on ASU (i % asu_count). Prefill reads any cached prefix KV, computes
    the remaining suffix, and writes the suffix KV; decode reads the layer KV, computes,
    then writes the new token KV.
    """
    if model is None:
        model = GLM_4_9B
    kv_per_token = kv_bytes_per_token(model)
    num_layers = model["num_layers"]
    asu_count = len(asu_ids)
    new_tokens = prompt_tokens - prefix_hit_tokens

    rows = []
    task_id = 0
    phase = 0

    def emit(src, dst, size, op, src_port, dst_port, dep):
        nonlocal task_id, phase
        rows.append([task_id, src, dst, size, op, priority, "0ns",
                     phase, dep, src_port, dst_port])
        task_id += 1
        phase += 1
        return str(phase - 1)

    for npu_id in npu_ids:
        prev = ""
        for layer in range(num_layers):
            asu_id = asu_ids[layer % asu_count]
            if prefix_hit_tokens > 0:
                prev = emit(npu_id, asu_id, prefix_hit_tokens * kv_per_token,
                            "URMA_READ", 0, 0, prev)
            prev = emit(npu_id, npu_id,
                        flops_to_ns(prefill_flops_per_layer(model, prefix_hit_tokens,
                                                            new_tokens), tflops),
                        "COMPUTE", "", "", prev)
            prev = emit(npu_id, asu_id, new_tokens * kv_per_token, "URMA_WRITE", 0, 0, prev)
        for step in range(decode_steps):
            context = prompt_tokens + step
            for layer in range(num_layers):
                asu_id = asu_ids[layer % asu_count]
                prev = emit(npu_id, asu_id, context * kv_per_token, "URMA_READ", 0, 0, prev)
                prev = emit(npu_id, npu_id,
                            flops_to_ns(decode_flops_per_layer(model, context), tflops),
                            "COMPUTE", "", "", prev)
                prev = emit(npu_id, asu_id, kv_per_token, "URMA_WRITE", 0, 0, prev)

    return rows


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--output-dir", required=True)
    p.add_argument("--asu-count", type=int, default=4)
    p.add_argument("--npu-count", type=int, default=8,
                   help="number of NPU servers; 8 makes the switch bandwidth 1:1, fewer oversubscribe (incast)")
    p.add_argument("--union-count", type=int, default=2)
    p.add_argument("--asu-bw", default="200Gbps")
    p.add_argument("--fabric-bw", default="400Gbps")
    p.add_argument("--npu-bw", default="200Gbps",
                   help="NPU NIC link bandwidth (Ethernet switch -> NPU)")
    p.add_argument("--transport-mode", default="RTP", choices=["RTP", "CTP"],
                   help="UB transport mode: RTP (reliable, DCQCN/CAQM) or CTP (compact, coarse "
                        "per-{dest Entity, VL} CNP rate control)")
    p.add_argument("--kv-bytes", default="4MB")
    p.add_argument("--reads-per-asu", type=int, default=1)
    p.add_argument("--workload", default="incast",
                   choices=["incast", "inference", "pure-incast", "multi-incast", "write"],
                   help="incast: flat NOF read storm; inference: per-layer compute + KV read/write; "
                        "pure-incast: every ASU URMA_WRITEs a chunk to one NPU concurrently; "
                        "multi-incast: NPU 0 reads every ASU (incast) while the other NPUs read only "
                        "ASU 0 (light flows that share the same priority/path, HOL victim probe)")
    p.add_argument("--model", default="glm-4-9b",
                   help="model geometry config name in models/ (e.g. glm-4-9b), read by --workload inference")
    p.add_argument("--incast-size", default="16MB",
                   help="pure-incast: bytes each ASU sends to the single NPU per port")
    p.add_argument("--prompt-tokens", type=int, default=1024,
                   help="inference prefill prompt length in tokens")
    p.add_argument("--prefix-hit-tokens", type=int, default=0,
                   help="cached prompt prefix tokens already in the KV cache; prefill reads them "
                        "from the ASU instead of recomputing (must be < --prompt-tokens)")
    p.add_argument("--decode-steps", type=int, default=4,
                   help="inference number of autoregressive decode steps")
    p.add_argument("--tflops", type=float, default=500.0,
                   help="NPU compute throughput (TFLOPS) used to turn per-layer FLOPs into a duration")
    p.add_argument("--ssd-read-latency", default="50us",
                   help="ASU SSD fixed read latency per transaction")
    p.add_argument("--ssd-write-latency", default="80us",
                   help="ASU SSD fixed write latency per transaction")
    p.add_argument("--ssd-bandwidth", default="12GB/s",
                   help="per-SSD sequential bandwidth (PCIe 5.0 x4 NVMe)")
    p.add_argument("--ssd-count", type=int, default=5,
                   help="number of SSDs per ASU; aggregate bandwidth = count x ssd-bandwidth")
    p.add_argument("--ssd-queue-count", type=int, default=8,
                   help="ASU SSD queue count (Jets mapped by jettyNum %% queue-count)")
    p.add_argument("--ctp-rate-ai", default="40Mbps",
                   help="CTP congestion-control additive rate increase")
    p.add_argument("--ctp-cnp-interval", default="50000ns",
                   help="CTP CNP minimum interval per {destination Entity, VL}")
    p.add_argument("--pri-high", type=int, default=1,
                   help="VL/priority for high-priority (foreground) ASU flows")
    p.add_argument("--pri-low", type=int, default=7,
                   help="VL/priority for low-priority (background) ASU flows")
    p.add_argument("--high-asu-count", type=int, default=None,
                   help="number of ASUs treated as high priority (default: half)")
    p.add_argument("--shared-pool-bytes", default="12582912",
                   help="UbQueueManager shared receive buffer for the union/1825 nodes; small "
                        "values make the 1825 backpressure propagate to the union so its egress "
                        "queue fills and it marks")
    p.add_argument("--cc-trace", action="store_true",
                   help="enable UB_CONGESTION_CONTROL_TRACE_ENABLE to emit DcqcnMarkTrace/CtpCnpTrace")
    p.add_argument("--no-cc", action="store_true",
                   help="disable active congestion control (UB_CC_ENABLED + CTP CcEnabled); "
                        "only PFC/CBFC backpressure remains")
    p.add_argument("--vl-scheduler", default="sp", choices=["sp", "dwrr"],
                   help="VL inter-scheduling at switch/DPU egress: sp (strict priority) or dwrr "
                        "(weighted round-robin)")
    p.add_argument("--dpu-role", default="transparent", choices=["transparent", "rp"],
                   help="DPU congestion-control role: transparent (pure forwarder) or rp "
                        "(RoCE reaction point that paces its RoCE egress)")
    p.add_argument("--rp-push", action="store_true",
                   help="RP actively pushes its per-flow rate back to each source ASU on every "
                        "allocation update")
    p.add_argument("--cqe", action="store_true",
                   help="send a CTP completion (CQE) from the NPU to the ASU on each URMA READ "
                        "completion, so the DPU piggybacks the per-flow rate onto it (use with "
                        "--dpu-role rp instead of --rp-push)")
    p.add_argument("--graded-backpressure", action="store_true",
                   help="scheme 3: DPU acts as a per-side CC terminus; when the union marks FECN on "
                        "ASU->NPU data from its local backlog, the DPU sends a local cut-CNP back to "
                        "the source ASU (per-hop local CC). Use with --dpu-role transparent.")
    p.add_argument("--union-kmin", default="5120",
                   help="union-only DCQCN Kmin in bytes (CBFC bus switches only; RoCE switches keep "
                        "the 5120 default). Lower to sensitize union per-NL FECN marking.")
    p.add_argument("--union-kmax", default="204800",
                   help="union-only DCQCN Kmax in bytes (CBFC bus switches only; default 204800)")
    p.add_argument("--union-pmax", default="0.01",
                   help="union-only DCQCN Pmax (CBFC bus switches only; default 0.01)")
    p.add_argument("--cc-disabled-nodes", default="",
                   help="comma-separated node IDs whose CTP senders ignore CNPs (CC-blind), "
                        "e.g. '0,1,2' to keep the heavy ASUs at line rate while the victim ASU "
                        "still consumes cut-CNPs")
    args = p.parse_args(argv)

    asu_port_count = args.union_count  # each ASU connects to every union
    kv = parse_bytes(args.kv_bytes)
    asu_bw_gbps = parse_bw_gbps(args.asu_bw)
    fabric_bw_gbps = parse_bw_gbps(args.fabric_bw)
    gateway_port_count = (args.asu_count * asu_bw_gbps) // fabric_bw_gbps
    if gateway_port_count < 1 or args.asu_count * asu_bw_gbps % fabric_bw_gbps != 0:
        sys.exit("asu_count * asu_bw must be a positive multiple of fabric_bw")

    high_asu_count = args.high_asu_count if args.high_asu_count is not None else args.asu_count // 2
    high_asu_count = max(0, min(high_asu_count, args.asu_count))

    def priority_for_asu(asu_idx):
        return args.pri_high if asu_idx < high_asu_count else args.pri_low

    cc_flag = "false" if args.no_cc else "true"

    os.makedirs(args.output_dir, exist_ok=True)

    # Node layout: each ASU has union_count ports (one 200G link to each union), each union
    # aggregates asu_count*asu_bw into gateway_port_count*fabric_bw (1:1), and each gateway
    # mirrors that capacity on its RoCE side into one Ethernet switch. Each NPU exposes a
    # single npu_bw (200G) NIC into that Ethernet switch -- the incast convergence point.
    asu_ids = list(range(0, args.asu_count))
    union_ids = list(range(args.asu_count, args.asu_count + args.union_count))
    gw_ids = list(range(args.asu_count + args.union_count,
                        args.asu_count + 2 * args.union_count))
    roce_switch_id = args.asu_count + 2 * args.union_count
    npu_ids = list(range(roce_switch_id + 1, roce_switch_id + 1 + args.npu_count))
    union_port_count = args.asu_count + gateway_port_count
    gw_port_count = 2 * gateway_port_count
    roce_port_count = args.union_count * gateway_port_count + args.npu_count

    # node.csv
    node_rows = [["nodeId", "nodeType", "portNum", "allocationDelay", "forwardDelay",
                  "rocePortCount", "flowControl"]]
    for nid in asu_ids:
        node_rows.append([nid, "DEVICE", asu_port_count, "1ns", "", "", ""])
    # The union is a lossless bus, not a store-and-forward switch: fixed 100 ns forwarding
    # delay, CBFC credit (lossless). It grants credit to the ASUs; the 1825 in turn grants
    # credit to the union. No extra node type is needed, only the 100 ns forwardDelay here.
    for nid in union_ids:
        node_rows.append([nid, "SWITCH", union_port_count, "1ns", "100ns", "", ""])
    for nid in gw_ids:
        node_rows.append([nid, "DPU", gw_port_count, "1ns", "", str(gateway_port_count), ""])
    node_rows.append([roce_switch_id, "SWITCH", roce_port_count, "1ns", "", "", "PFC_DYNAMIC"])
    for nid in npu_ids:
        node_rows.append([nid, "DEVICE", 1, "1ns", "", "", "PFC_DYNAMIC"])

    # topology.csv
    topo_rows = [["nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay"]]
    for asu_id in asu_ids:
        for uidx, union_id in enumerate(union_ids):
            topo_rows.append([asu_id, uidx, union_id, asu_id, args.asu_bw, "20ns"])
    for gidx, gw_id in enumerate(gw_ids):
        for k in range(gateway_port_count):
            topo_rows.append([union_ids[gidx], args.asu_count + k, gw_id, k,
                              args.fabric_bw, "20ns"])
            topo_rows.append([gw_id, gateway_port_count + k, roce_switch_id,
                              gidx * gateway_port_count + k, args.fabric_bw, "20ns"])
    for nidx, npu_id in enumerate(npu_ids):
        topo_rows.append([roce_switch_id, args.union_count * gateway_port_count + nidx,
                          npu_id, 0, args.npu_bw, "20ns"])

    # routing_table.csv. Each ASU port j is reachable only through union j; the ASU
    # reaches the NPU through either port (ECMP across its two unions).
    def ports_with_metrics(ports, metric):
        return " ".join(str(p) for p in ports), " ".join(str(metric) for _ in ports)

    rt_rows = [["nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"]]
    asu_out, asu_metric = ports_with_metrics(range(asu_port_count), 4)
    for asu_id in asu_ids:
        for npu_id in npu_ids:
            rt_rows.append([asu_id, npu_id, 0, asu_out, asu_metric])
    for uidx, union_id in enumerate(union_ids):
        for asu_id in asu_ids:
            rt_rows.append([union_id, asu_id, uidx, asu_id, 1])
        gw_ports = list(range(args.asu_count, args.asu_count + gateway_port_count))
        gw_out, gw_metric = ports_with_metrics(gw_ports, 2)
        for npu_id in npu_ids:
            rt_rows.append([union_id, npu_id, 0, gw_out, gw_metric])
    for gidx, gw_id in enumerate(gw_ids):
        for asu_id in asu_ids:
            rt_rows.append([gw_id, asu_id, gidx, 0, 2])
        roce_ports = list(range(gateway_port_count, 2 * gateway_port_count))
        roce_out, roce_metric = ports_with_metrics(roce_ports, 1)
        for npu_id in npu_ids:
            rt_rows.append([gw_id, npu_id, 0, roce_out, roce_metric])
    for gidx in range(args.union_count):
        for asu_id in asu_ids:
            sw_gw = list(range(gidx * gateway_port_count, (gidx + 1) * gateway_port_count))
            sw_out, sw_metric = ports_with_metrics(sw_gw, 3)
            rt_rows.append([roce_switch_id, asu_id, gidx, sw_out, sw_metric])
    for nidx, npu_id in enumerate(npu_ids):
        rt_rows.append([roce_switch_id, npu_id, 0,
                        args.union_count * gateway_port_count + nidx, 1])
    for npu_id in npu_ids:
        for asu_id in asu_ids:
            for port in range(asu_port_count):
                rt_rows.append([npu_id, asu_id, port, 0, 4])

    # transport_channel.csv: one TP channel per (ASU port, NPU).
    tc_rows = [["nodeId1", "portId1", "tpn1", "nodeId2", "portId2", "tpn2",
                "priority", "metric"]]
    for npu_idx, npu_id in enumerate(npu_ids):
        for asu_idx, asu_id in enumerate(asu_ids):
            pri = priority_for_asu(asu_idx)
            for port in range(asu_port_count):
                tc_rows.append([asu_id, port, npu_idx * asu_port_count + port, npu_id, 0,
                                asu_idx * asu_port_count + port, pri, 4])

    # traffic.csv: NOF read, each NPU reads every ASU on each of its ports.
    tf_rows = [["taskId", "sourceNode", "destNode", "dataSize(Byte)", "opType",
                "priority", "delay", "phaseId", "dependOnPhases", "srcPortId", "dstPortId"]]
    if args.workload == "inference":
        if not (0 <= args.prefix_hit_tokens < args.prompt_tokens):
            sys.exit("--prefix-hit-tokens must satisfy 0 <= prefix < prompt-tokens")
        model = load_model_config(args.model)
        tf_rows.extend(gen_inference_traffic(
            npu_ids, asu_ids, args.prompt_tokens, args.decode_steps, args.tflops,
            args.pri_high, args.prefix_hit_tokens, model))
    elif args.workload == "pure-incast":
        if len(npu_ids) != 1:
            sys.exit("--workload pure-incast requires --npu-count 1 (single sink)")
        size = parse_bytes(args.incast_size)
        task_id = 0
        for asu_id in asu_ids:
            for port in range(asu_port_count):
                tf_rows.append([task_id, asu_id, npu_ids[0], size, "URMA_WRITE",
                                args.pri_high, "10ns", 0, "", port, 0])
                task_id += 1
    elif args.workload == "multi-incast":
        if len(npu_ids) < 2:
            sys.exit("--workload multi-incast requires --npu-count >= 2")
        task_id = 0
        # NPU0 (incast) reads the high-ASU-index group (0..high_asu_count-1); NPU1
        # (victim) reads the first low-index ASU. Victim/incast are split by ASU index so they
        # can share one VL (pri_high == pri_low) and still be distinct flows.
        for npu_idx, npu_id in enumerate(npu_ids):
            if npu_idx == 0:
                targets = [(asu_ids[asu_idx], priority_for_asu(asu_idx))
                           for asu_idx in range(high_asu_count)]
            else:
                targets = [(asu_ids[high_asu_count], priority_for_asu(high_asu_count))]
            for asu_id, pri in targets:
                for port in range(asu_port_count):
                    for _ in range(args.reads_per_asu):
                        tf_rows.append([task_id, npu_id, asu_id, kv, "URMA_READ",
                                        pri, "10ns", 0, "", 0, port])
                        task_id += 1
    elif args.workload == "write":
        # Write path: NPU -> ASU. Data flows NPU -> switch -> 1825 -> union -> ASU,
        # the reverse of the read direction. Congestion lands on the ASU SSD.
        task_id = 0
        for npu_id in npu_ids:
            for asu_id in asu_ids:
                for port in range(asu_port_count):
                    for _ in range(args.reads_per_asu):
                        tf_rows.append([task_id, npu_id, asu_id, kv, "URMA_WRITE",
                                        args.pri_high, "10ns", 0, "", 0, port])
                        task_id += 1
    else:
        task_id = 0
        for npu_id in npu_ids:
            for asu_idx, asu_id in enumerate(asu_ids):
                pri = priority_for_asu(asu_idx)
                for port in range(asu_port_count):
                    for _ in range(args.reads_per_asu):
                        tf_rows.append([task_id, npu_id, asu_id, kv, "URMA_READ", pri, "10ns",
                                        0, "", 0, port])
                        task_id += 1

    def write_rows(name, rows):
        with open(os.path.join(args.output_dir, name), "w", newline="") as f:
            w = csv.writer(f)
            for r in rows:
                w.writerow([str(c) for c in r])

    write_rows("node.csv", node_rows)
    write_rows("topology.csv", topo_rows)
    write_rows("routing_table.csv", rt_rows)
    write_rows("transport_channel.csv", tc_rows)
    write_rows("traffic.csv", tf_rows)

    with open(os.path.join(args.output_dir, "network_attribute.txt"), "w") as f:
        f.write(DEFAULT_NETWORK_ATTRIBUTES.format(fabric_bw=args.fabric_bw,
                                                  transport_mode=args.transport_mode,
                                                  cqe_enabled="true" if args.cqe else "false",
                                                  vl_scheduler=args.vl_scheduler.upper(),
                                                  dpu_role="TRANSPARENT" if args.dpu_role == "transparent" else "RP",
                                                  graded_bp_enabled="true" if args.graded_backpressure else "false",
                                                  rp_push_enabled="true" if args.rp_push else "false",
                                                  rp_cqe_fallback_enabled="true" if args.cqe else "false",
                                                  ctp_rate_ai=args.ctp_rate_ai,
                                                  ctp_cnp_interval=args.ctp_cnp_interval,
                                                  ctp_cc_enabled=cc_flag,
                                                  ub_cc_enabled=cc_flag,
                                                  cc_disabled_nodes=args.cc_disabled_nodes,
                                                  ssd_read_latency=args.ssd_read_latency,
                                                  ssd_write_latency=args.ssd_write_latency,
                                                  ssd_bandwidth=scale_bandwidth(args.ssd_bandwidth,
                                                                               args.ssd_count),
                                                  ssd_queue_count=args.ssd_queue_count,
                                                  shared_pool_bytes=args.shared_pool_bytes,
                                                  union_kmin=args.union_kmin,
                                                  union_kmax=args.union_kmax,
                                                  union_pmax=args.union_pmax,
                                                  cc_trace_enable="true" if args.cc_trace else "false"))

    print(f"Generated case in {args.output_dir}: {args.asu_count} ASU, "
          f"{args.union_count} union, {args.union_count} DPU, {args.npu_count} NPU "
          f"({args.workload})")


if __name__ == "__main__":
    main(sys.argv[1:])
