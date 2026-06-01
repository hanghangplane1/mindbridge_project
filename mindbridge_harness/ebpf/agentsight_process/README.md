# MindBridge AgentSight-Derived eBPF Monitors

This directory vendors AgentSight C/libbpf tracers as the real MindBridge eBPF
collection backend.

It is intentionally limited to the C/libbpf monitor layer:

- kernel eBPF program: `process_new.bpf.c`
- userspace libbpf loader: `process_new.c`
- process/file/network/resource helper headers under `process_ext/`
- stdio payload capture: `stdiocap.bpf.c`, `stdiocap.c`
- TLS plaintext capture: `sslsniff.bpf.c`, `sslsniff.c`

MindBridge does not vendor AgentSight's Rust collector or Next.js frontend.
The higher-level run ownership, correlation, artifact writing, Gateway API,
and dashboard are implemented by the MindBridge C++ Harness.

## Build

The build requires Linux plus `clang`, `bpftool`, `libbpf`, `libelf`, and `zlib`.

```bash
make -C mindbridge_harness/ebpf/agentsight_process process_new
make -C mindbridge_harness/ebpf/agentsight_process stdiocap
make -C mindbridge_harness/ebpf/agentsight_process sslsniff
```

When available, CMake with `-DMINDBRIDGE_ENABLE_EBPF=ON` builds this helper and
copies the helpers as `mindbridge_ebpf_monitor`, `mindbridge_stdiocap_monitor`,
and `mindbridge_sslsniff_monitor`. If dependencies are not available, the
C++ placeholder adapter still builds so the default MindBridge verification path
does not fail on non-eBPF machines.

## Runtime

```bash
export MINDBRIDGE_EBPF_ENABLED=1
export MINDBRIDGE_EBPF_BINARY=./build-ebpf/mindbridge_harness/mindbridge_ebpf_monitor
export MINDBRIDGE_EBPF_TRACE_TLS=1
export MINDBRIDGE_EBPF_TLS_BINARY=./build-ebpf/mindbridge_harness/mindbridge_sslsniff_monitor
export MINDBRIDGE_EBPF_TLS_COMMANDS=curl
```

The helper writes JSON lines to stdout. `mindbridge::observability::EbpfMonitor`
reads that stream and writes `.mindbridge/runs/<run_id>/ebpf_events.jsonl`.
TLS plaintext capture must be filtered by PID or command before runtime starts it.
