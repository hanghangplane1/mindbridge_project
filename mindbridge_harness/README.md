# MindBridge C++ Harness

本目录实现 **MindBridge C++ Harness**：在保留现有 [examples/mindcare](../examples/mindcare/) 可运行 demo 的前提下，抽取 `ModelClient` / `VectorStoreClient`、注册表与 `AgentRouter` 集成、工具注册与权限、Hooks、Trace、`AgentLoop`，以及 **hardness** 评测与 **Nginx** 配置模板。

## 与 examples/mindcare 的关系

- **请勿删除或弱化** `examples/mindcare/`：该目录仍是已调通的参考实现与对照 demo。
- 新逻辑优先放在 `mindbridge_harness/`；`ai_counselor_agent` 仅增加对 `RegistryAgentRouter` 的依赖，用于在有多 evaluator 实例时做路由，单实例时退化为直接命中唯一节点，多实例时可按 `contextId` 做稳定路由。

## 组件说明

| 组件 | 说明 |
|------|------|
| `mindbridge::ModelClient` | 模型抽象；支持 Ollama 与 OpenAI-compatible，demo 默认按环境自动选择。 |
| `mindbridge::VectorStoreClient` | 向量库抽象；`ChromaVectorStore` 实现 Chroma v1 query。 |
| `mindbridge::RegistryAgentRouter` | 从 `RegistryClient` 拉取全量 Agent，同步到 `agent_rpc::orchestrator::AgentRouter`，先按 skill/tag 过滤，再按策略（如一致性哈希）选择 URL。 |
| `ToolRegistry` / `PermissionChecker` / `HookExecutor` / `TraceRecorder` / `AgentLoop` | Harness 横切能力。 |
| `mindbridge::net::AsyncHttpServer` | 正式入口服务端：Boost.Asio/Beast async accept/read/write，多 worker，支持 JSON 与 SSE。 |
| `mindbridge::auth::AuthService` | AK/SK 登录与 SQLite 会话索引；Gateway 可用 token 隔离不同用户的 conversation/run artifacts。 |
| `mindbridge_net_demo` | 网络能力 demo/load client：配置驱动模拟多客户端 HTTP/SSE/WebSocket、auth、heartbeat 和 subscribe。 |
| `mindbridge_gateway` | 正式 HTTP 网关：健康检查、request_id 注入、轻量鉴权占位，将 JSON-RPC 转发到 orchestrator；支持 `MINDBRIDGE_ORCHESTRATOR_URLS` 多副本健康路由。 |
| `mindbridge_orchestrator` | 正式编排入口：接收 gateway 请求，按 `contextId` 一致性哈希路由 counselor/evaluator 多副本，并通过 Asio/Beast 网络层做健康探测、熔断和恢复。 |
| `mindbridge_counselor` | 正式咨询 Agent：主链路接入 `AgentLoop`、`PromptPolicy`、`ToolRegistry`、权限、Hooks 与 Trace。 |
| `mindbridge_evaluator` | 正式风险评估 Agent：使用 evaluator prompt 和风险兜底策略，可触发 MCP 邮件预警工具。 |
| `mindbridge_benchmark` | 读取 `benchmarks/mindbridge_tasks.json`，执行 HTTP 探测与 hardness 计分。 |
| `BoundaryObservability` | 可选 eBPF 边界观测：关联 MindBridge 语义 trace 与 eBPF 动作流，生成 run 级观测 artifacts 和前端调试视图。 |
| `configs/nginx.conf` | Nginx 模板：`/` 静态、`/api/` 反代 gateway、关闭 buffering 以支持 SSE。 |

## 构建

在仓库根目录 `build/` 下：

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target mindbridge_harness mindbridge_gateway mindbridge_orchestrator mindbridge_counselor mindbridge_evaluator mindbridge_benchmark ai_counselor_agent -j
```

## 运行网关（可选）

```bash
export MINDBRIDGE_ORCHESTRATOR_URL=http://127.0.0.1:5009
./mindbridge_harness/mindbridge_gateway 8090
```

Nginx 将 `8088` 指到 gateway 的 `8090` 即可（见 `configs/nginx.conf`）。
Orchestrator 可通过逗号分隔 URL 列表启用多副本稳定路由：

```bash
export MINDBRIDGE_COUNSELOR_URLS=http://127.0.0.1:5010,http://127.0.0.1:5012,http://127.0.0.1:5013
export MINDBRIDGE_EVALUATOR_URLS=http://127.0.0.1:5011,http://127.0.0.1:5014,http://127.0.0.1:5015
```

Gateway 也可配置多个 orchestrator：

```bash
export MINDBRIDGE_ORCHESTRATOR_URLS=http://127.0.0.1:5009,http://127.0.0.1:5019
```

Gateway 也提供云存储上传入口。默认开发后端写入 `.mindbridge/storage`，用于不依赖
Redis/MySQL/FastDFS 的端到端验证；真实 live 后端由 MindBridge 项目内
`infra/cloud_storage/docker-compose.yaml` 提供，默认暴露 MySQL `3307`、Redis `6379`、
FastDFS HTTP `80` 和 HTTPS `443`。

```bash
export MINDBRIDGE_STORAGE_ROOT=.mindbridge/storage
curl -X POST http://127.0.0.1:8090/api/storage/status
```

```bash
bash scripts/start_mindbridge_cloud_storage.sh
source .mindbridge/cloud_storage/live.env
bash scripts/verify_cloud_storage_live.sh
bash scripts/verify_state_mysql_live.sh
```

兼容旧接口 `/api/md5`、`/api/upload`、`/api/chunk_init`、`/api/chunk_upload`、
`/api/chunk_merge`，同时提供 `/api/storage/*` 别名。上传成功的文件元数据会通过
`run_artifact` namespace 写入分布式状态层，便于 follower replay 和审计。

正式 `mindbridge_counselor` 默认启动 master/follower 两个状态库。master 由
`MINDBRIDGE_STATE_DB_PATH` / `MINDBRIDGE_STATE_NODE_ID` 控制，follower 由
`MINDBRIDGE_STATE_FOLLOWER_DB_PATH` / `MINDBRIDGE_STATE_FOLLOWER_NODE_ID` 控制，
后台 `ReplicaWorker` 按 `MINDBRIDGE_STATE_REPLICA_POLL_MS` 周期增量 replay。
可通过 `GET /api/state/replication/status` 查看同步状态。
Gateway 的 `/api/conversations` 直接读取 master `DistributedStateStore` 中的
`chat_memory` / `consult_memory` / `risk_memory`，不再读独立 SQLite 历史表；
默认使用同一个 `MINDBRIDGE_STATE_DB_PATH`，从而保证网页 Stored History 与
Counselor 写入的状态链路一致。

网络层健康治理参数包括 `MINDBRIDGE_HEALTH_INTERVAL_MS`、`MINDBRIDGE_HEALTH_FAIL_THRESHOLD`、
`MINDBRIDGE_HEALTH_RECOVER_THRESHOLD`、`MINDBRIDGE_CIRCUIT_OPEN_MS`、
`MINDBRIDGE_NET_TLS_VERIFY` 和 `MINDBRIDGE_NET_CA_FILE`。服务端 Asio worker 和请求限制可通过
`MINDBRIDGE_NET_IO_THREADS`、`MINDBRIDGE_NET_MAX_REQUEST_BYTES`、
`MINDBRIDGE_NET_REQUEST_TIMEOUT_SEC` 配置。

## 登录与用户隔离（可选）

Gateway 支持 AK/SK 登录换短期 session token。默认 `MINDBRIDGE_AUTH_REQUIRED=false`，保持旧 demo
和 benchmark 兼容；设置为 `true` 后，聊天、会话列表和 run artifact 查询必须带 token。

```bash
export MINDBRIDGE_AUTH_REQUIRED=true
export MINDBRIDGE_AUTH_DB_PATH=.mindbridge/mindbridge.db
export MINDBRIDGE_AUTH_BOOTSTRAP_USERS='[{"user_id":"demo_user","display_name":"Demo User","role":"user","access_key_id":"...","secret_key":"..."}]'
```

`MINDBRIDGE_AUTH_BOOTSTRAP_USERS` 只用于运行时注入初始用户，请勿把真实 AK/SK 写入代码、
配置、脚本、文档、提交信息或日志。Secret 会以 salted SHA-256 hash 保存到 SQLite。
登录接口为 `/api/auth/login`，返回 token 后前端只保存 token；Gateway 会把前端传入的
`contextId` 改写为用户作用域内的 conversation id，并把 run ownership 记录到 SQLite，
因此不同用户使用同名会话也不会看到彼此的对话和 run artifacts。

## 运行 Net Demo（可选）

`mindbridge_net_demo` 用于验证当前 Asio/Beast 网络层的 HTTP/SSE/WebSocket 能力。它参考
`demonet.txt` 中的多客户端、长连接、认证消息、心跳、订阅和错误回调模型，但不导入其中的
Hik 登录、AES token 或 XML 业务协议。

```bash
cmake --build build --target mindbridge_net_demo mindbridge_gateway -j2
./build/mindbridge_harness/mindbridge_gateway 8090
./build/mindbridge_harness/mindbridge_net_demo mindbridge_harness/configs/net_demo.json
```

默认配置位于 `configs/net_demo.json`，可调整 `client_count`、`request_frequency_ms`、
`enable_http`、`enable_sse`、`enable_websocket` 和 `enable_subscribe`。WebSocket demo
端点是 gateway 的 `/api/demo/ws`，仅用于演示和压测，不作为正式业务认证入口。

## 远程模型配置

```bash
export MINDBRIDGE_MODEL_BASE_URL=https://your-openai-compatible-endpoint
export MINDBRIDGE_MODEL_API_KEY=...
export MINDBRIDGE_MODEL_NAME=qwen3.6-plus
```

`MINDBRIDGE_MODEL_PROVIDER` 默认是 `auto`：检测到 `DASHSCOPE_API_KEY` 且未显式设置
`MINDBRIDGE_MODEL_BASE_URL` 时使用 DashScope 原生 HTTP endpoint；检测到其他
`MINDBRIDGE_MODEL_BASE_URL` 或 `MINDBRIDGE_MODEL_API_KEY` 时使用 OpenAI-compatible；
否则使用本地 Ollama。Base URL 可写到域名根路径，也可写到 `/v1`，运行时会统一
拼接 chat/embedding endpoint。

API Key 只从环境变量读取，不写入源码、配置文件或日志。

如果只想走远程 API，不希望 fallback 到本地 Ollama，使用：

```bash
bash scripts/start_demo_dashscope.sh
# 或
bash scripts/start_demo_openai.sh
```

这两个入口会设置 `MINDBRIDGE_REQUIRE_REMOTE_MODEL=1`。DashScope 入口会先做一次
API key preflight，401 会在启动前失败并提示更换有效 key，而不是等浏览器刷新后才报错。

## eBPF 边界观测（可选）

默认关闭，不影响普通 demo 和验证：

```bash
cmake -S . -B build-ebpf -DBUILD_TESTING=OFF -DMINDBRIDGE_ENABLE_EBPF=ON
cmake --build build-ebpf --target mindbridge_ebpf_monitor mindbridge_stdiocap_monitor mindbridge_sslsniff_monitor mindbridge_counselor -j2
export MINDBRIDGE_EBPF_ENABLED=1
export MINDBRIDGE_EBPF_BINARY=./build-ebpf/mindbridge_harness/mindbridge_ebpf_monitor
```

每次 run 会额外写入 `ebpf_events.jsonl`、`boundary_trace.jsonl` 和
`observability_report.json`。`boundary_trace.jsonl` 是降噪后的分析视图：
`BoundaryTraceCorrelator` 在 run 完成后按 `CLOCK_SYNC` 时间对齐、默认 500ms 时间窗口、
参数引用匹配和 PID/PPID 进程血缘关联模型事件与系统动作；未关联的原始明细仍保留在
`ebpf_events.jsonl`。前端 `Observability` tab 通过 Gateway 的
`/api/demo/runs/latest/observability` 查看采集点全景、进程树、可过滤/缩放时间线、
CPU/Memory 轻量 dashboard、TLS 明文片段和事件日志。

真实内核采集源码位于 `mindbridge_harness/ebpf/agentsight_process/`，来源是
AgentSight 的 `bpf/process_new.*` 进程追踪器、扩展头文件、`stdiocap.*` 标准输入输出捕获器和
`sslsniff.*` TLS plaintext 捕获器。CMake 在检测到
`clang`、`bpftool`、`libbpf`、`libelf`、`zlib` 时会构建该 helper；依赖缺失时才退回
占位 adapter，以保证非 eBPF 机器仍可运行默认验证。
loader 默认加载进程生命周期和 file-open 基础探针；文件系统、网络、信号、内存和
CoW 扩展探针通过对应 `--trace-*` 参数在 load 前启用，避免未启用能力仍进入
kernel verifier。Ubuntu 20.04 / Linux 5.15 环境已验证默认模式可 live attach，
`--trace-fs` 可采集 `DIR_CREATE`、`FILE_TRUNCATE`、`FILE_RENAME` 和 `FILE_DELETE`
摘要事件。
可用 `bash scripts/verify_ebpf_live.sh` 做真实内核 live 验证；该脚本会构建 helper，
再分别检查默认基础探针、fs/net/signals/mem 扩展探针、MindBridge runtime artifacts
以及 `stdiocap` stdout/stderr payload 捕获和 `sslsniff` OpenSSL TLS 明文捕获。

MindBridge runtime 可通过环境变量把扩展采集传给真实 helper：

```bash
export MINDBRIDGE_EBPF_TRACE_FS=1
export MINDBRIDGE_EBPF_TRACE_NET=1
export MINDBRIDGE_EBPF_TRACE_SIGNALS=1
export MINDBRIDGE_EBPF_TRACE_MEM=1
export MINDBRIDGE_EBPF_TRACE_RESOURCES=1
export MINDBRIDGE_EBPF_TRACE_TLS=1
export MINDBRIDGE_EBPF_TLS_BINARY=./build-ebpf/mindbridge_harness/mindbridge_sslsniff_monitor
export MINDBRIDGE_EBPF_TLS_COMMANDS=curl   # TLS 明文捕获必须限制 PID 或 command
# 可选：限制到指定进程、命令或 cgroup
export MINDBRIDGE_EBPF_PID=1234
export MINDBRIDGE_EBPF_COMMANDS=mindbridge_counselor,mindbridge_gateway
export MINDBRIDGE_EBPF_CGROUP_PATH=/sys/fs/cgroup/...
# 可选：调试时间线正文片段，默认关闭以保护咨询内容
export MINDBRIDGE_OBSERVABILITY_CAPTURE_TEXT=1
export MINDBRIDGE_OBSERVABILITY_TEXT_LIMIT=512
export MINDBRIDGE_OBS_CORRELATION_WINDOW_MS=500
```

DashScope/Qwen 推荐配置：

```bash
export DASHSCOPE_API_KEY=...
export MINDBRIDGE_MODEL_PROVIDER=dashscope_native
export MINDBRIDGE_MODEL_NAME=qwen-plus
export MINDBRIDGE_MULTIMODAL_PROVIDER=dashscope
export MINDBRIDGE_ASR_PROVIDER=dashscope
export MINDBRIDGE_DASHSCOPE_ASR_API_KEY="$DASHSCOPE_API_KEY"
export MINDBRIDGE_TTS_PROVIDER=dashscope
```

如果显式设置 `MINDBRIDGE_MODEL_PROVIDER=openai_compatible`，再配置
`MINDBRIDGE_MODEL_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1`。
