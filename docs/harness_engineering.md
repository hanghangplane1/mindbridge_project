# Harness Engineering 实践（MindBridge）

## 为什么需要 Harness

裸 LLM 调用在心理健康场景不可控：回复风格波动、风险边界不稳定、工具动作缺审计。  
MindBridge 通过 C++ Harness Runtime 做约束工程，让模型成为可治理的系统组件。

## 关键约束层

### 1. PromptPolicy（输入约束）
- 按 `CHAT / CONSULT / RISK / EVALUATOR / SESSION_EVALUATOR` 分场景构建系统提示词。
- 用 Prompt 约束替代 LoRA 主链路，降低部署复杂度。
- `CONSULT` 回复默认要求承接当前情绪、结合上下文给出 3 到 4 条具体下一步，覆盖可立即尝试的小行动、后续观察信号和求助边界；感谢、好转或准备尝试这类多轮收尾发言也要继续给出巩固练习和复发预案，而不是只做简短结束语。知识库检索结果会显式标为 MindBridge RAG 参考内容；回复校验会拒绝把用户最新发言原样复读成助手回复，并触发一次带纠错说明的重试。

### 2. ToolRegistry（动作约束）
- 外部动作必须通过已注册工具触发。
- 核心工具包括：
  - `retrieve_knowledge`
  - `assess_risk`
  - `analyze_multimodal_emotion`
  - `recognize_speech`
  - `synthesize_speech`
  - `conversation_memory_read`
  - `conversation_memory_write`
  - `mcp_excel_write`
  - `mcp_email_alert`

### 3. PermissionChecker（权限约束）
- 区分只读工具与可变更工具。
- 白名单/拒绝名单优先于默认模式。
- 阻止未经授权的外部动作执行。

### 4. HookExecutor + TraceRecorder（可观测约束）
- 记录 `pre_tool_use`、`post_tool_use`、生成过程和拒绝事件。
- Trace 输出 JSONL，支持复盘和评测联动。
- Runtime/main loop 必须保留 `run_started`、`model_requested`、`run_finished` 证据链；启用 eBPF 时只向主 trace 追加 `ebpf_started`、`ebpf_unavailable`、`correlation_finished` 等生命周期摘要。

### 4.1 BoundaryObservability（eBPF 边界观测）
- eBPF 观测默认关闭，通过 `MINDBRIDGE_EBPF_ENABLED=1` 启用；构建 helper 需要 `-DMINDBRIDGE_ENABLE_EBPF=ON`。
- 低层采集 vendoring 自 AgentSight：`mindbridge_harness/ebpf/agentsight_process/process_new.bpf.c` 是进程/文件/网络/资源内核态探针，`process_new.c` 是 libbpf 用户态 loader；`stdiocap.bpf.c` / `stdiocap.c` 提供 stdin/stdout/stderr payload 捕获；`sslsniff.bpf.c` / `sslsniff.c` 通过 OpenSSL/GnuTLS/NSS uprobe 捕获 TLS 加密前/解密后的明文。MindBridge 不引入 AgentSight Rust collector。
- loader 默认只加载进程生命周期、bash readline 和 file-open 基础探针；`--trace-fs`、`--trace-net`、`--trace-signals`、`--trace-mem`、`--trace-cow` 对应的扩展探针必须在 `bpf_object__load` 前通过 autoload 开关显式启用，避免 disabled program 仍被 verifier 预加载。
- `EbpfMonitor` 在 run 开始后启动 helper，读取 JSONL 动作流并写入 `ebpf_events.jsonl`；权限不足、helper 缺失或平台不支持时必须降级为 `ebpf_unavailable`，不能阻断咨询主链路。
- C++ runtime 启动真实 helper 时必须使用 AgentSight `process_new` 支持的 CLI：`-m`、`-p`、`-c`、`--trace-fs`、`--trace-net`、`--trace-signals`、`--trace-mem`、`--trace-cow`、`--trace-resources`、`--cgroup-filter`。不要把 placeholder adapter 的 `--run-id`、`--scope` 等参数传给真实 helper；run_id 由 `EbpfMonitor` 写 artifact 时补充。TLS 明文捕获作为第二 helper 启动，使用 `sslsniff` 的 `-p` / `-c` 过滤参数。
- eBPF 扩展采集通过环境变量打开：`MINDBRIDGE_EBPF_TRACE_FS`、`MINDBRIDGE_EBPF_TRACE_NET`、`MINDBRIDGE_EBPF_TRACE_SIGNALS`、`MINDBRIDGE_EBPF_TRACE_MEM`、`MINDBRIDGE_EBPF_TRACE_COW`、`MINDBRIDGE_EBPF_TRACE_RESOURCES`、`MINDBRIDGE_EBPF_TRACE_TLS`。`MINDBRIDGE_EBPF_TRACE_ALL=1` 等价于打开 fs/net/signals/mem，但不包含高开销 CoW，也不包含敏感的 TLS 明文捕获。
- `MINDBRIDGE_EBPF_TRACE_TLS=1` 必须配合 `MINDBRIDGE_EBPF_PID` 或 `MINDBRIDGE_EBPF_TLS_COMMANDS`，避免默认扫描全机 HTTPS 明文；显式设置 `MINDBRIDGE_EBPF_TLS_COMMANDS` 时 TLS helper 按 command 过滤，否则按 PID 过滤。TLS helper 路径可用 `MINDBRIDGE_EBPF_TLS_BINARY` 覆盖。写入 artifact 前会保留 run_id/helper 字段，前端 Observability tab 显示 `TLS Plaintext` 边界卡、TLS 事件数和明文片段。
- 真实内核验证使用 `bash scripts/verify_ebpf_live.sh`，覆盖默认 autoload、进程/file-open 基础面、fs/net/signals/mem 扩展事件、MindBridge runtime artifact 集成、`stdiocap` stdout/stderr payload 捕获，以及 `sslsniff` OpenSSL TLS plaintext 捕获。
- `BoundaryTraceCorrelator` 读取 `trace.jsonl` 与 `ebpf_events.jsonl`，输出降噪后的 `boundary_trace.jsonl` 和 `observability_report.json`。关联规则是 run 后生成的轻量启发式：`CLOCK_SYNC` 时间对齐、默认 500ms 时间窗口、prompt/response/tool 引用与 syscall 参数匹配、PID/PPID 进程血缘；窗口可用 `MINDBRIDGE_OBS_CORRELATION_WINDOW_MS` 覆盖。未关联普通 action 只在 `correlation_summary.suppressed_action_events` 计数，原始明细仍保留在 `ebpf_events.jsonl`。
- `MINDBRIDGE_EBPF_TRACE_RESOURCES=1` 会生成 `RESOURCE_SAMPLE`；report 必须汇总 `performance_metrics`，前端展示 AVG/PEAK CPU、AVG/PEAK Memory 和内存趋势。
- 默认不保存完整 prompt、API key、Authorization header、心理咨询正文或 HTTPS 明文 payload；疑似 secret 字段必须脱敏。只有显式设置 `MINDBRIDGE_OBSERVABILITY_CAPTURE_TEXT=1` 时，trace 才允许保存截断的 prompt/response/user message excerpt；长度由 `MINDBRIDGE_OBSERVABILITY_TEXT_LIMIT` 控制，默认 512 字节。

### 5. RuntimeContext + ConversationMemory（上下文约束）
- 请求上下文结构化：`request_id`、`conversation_id`、`intent`、`risk_level`。
- Gateway 开启 `MINDBRIDGE_AUTH_REQUIRED=true` 后必须先通过 AK/SK 登录换取 session token；token 校验成功后注入 `user_id`，并把前端 `contextId` 改写为用户作用域内的 conversation id。
- 用户隔离索引保存在 SQLite：run ownership 按 `user_id` 过滤；runtime artifact 仍写入 `.mindbridge/runs/<run_id>/`，但 Gateway 查询层必须校验 owner。正式网页对话历史不再写独立 SQLite 历史表，`/api/conversations` 直接按 `user_id` 从 `DistributedStateStore` 的会话记忆 namespace 读取。非强制登录 demo 以 `anonymous` 身份查询同一状态链路。
- 只注入最近历史，避免上下文过载。
- 上下文裁剪必须保持 UTF-8 边界安全，避免中文历史被按字节截断后导致 JSON request 序列化失败。
- HTTP/SSE 服务端必须忽略断连写入触发的 `SIGPIPE`，浏览器刷新、curl 超时或前端取消流式请求不能导致 Gateway/Orchestrator/Counselor 进程退出。
- Gateway、Orchestrator、Counselor、Evaluator 的正式入口服务端必须走 `mindbridge::net::AsyncHttpServer`，使用 Asio/Beast `async_accept/read/write`，通过 `MINDBRIDGE_NET_IO_THREADS` 控制 worker 数；旧 demo `http_server.hpp` 只保留给 legacy/demo 代码。
- Gateway/Orchestrator 下游通信走 `mindbridge::net`，使用 Asio/Beast 支持 HTTP/HTTPS、endpoint 健康探测、失败阈值摘除、熔断窗口和恢复探测；Evaluator 这类可能触发外部动作的请求不做透明重复投递。
- `mindbridge_net_demo` 只验证网络层能力：配置驱动、多客户端 HTTP/SSE/WebSocket、auth、heartbeat、subscribe、error callback 和 reconnect。第三方 OCR demo 里的登录、AES 和 XML 业务协议不得进入 MindBridge 主链路。
- AK/SK 只作为登录凭证，不在浏览器长期保存；前端保存短期 token，secret 只通过登录请求进入 Gateway，且不得写入仓库文件或日志。

### 5.1 DistributedStateStore（状态复制约束）
- `DistributedStateStore` 是 ConversationMemory 等运行时状态的复制层；正式 Counselor 默认启用，可通过 `MINDBRIDGE_STATE_STORE_ENABLED=false` 临时关闭。
- 默认 master SQLite 路径是 `.mindbridge/state/counselor_master.sqlite`，节点名默认 `counselor-master`；可用 `MINDBRIDGE_STATE_DB_PATH` 和 `MINDBRIDGE_STATE_NODE_ID` 覆盖。设置 `MINDBRIDGE_STATE_BACKEND=mysql` 时使用 MySQL 的 `mb_state_records`、`mb_state_changes`、`mb_replica_progress`，连接参数复用 `MINDBRIDGE_MYSQL_*`。
- 正式 Counselor 默认同时启动 follower store：默认 SQLite 路径 `.mindbridge/state/counselor_follower.sqlite`，节点名 `counselor-follower`；可用 `MINDBRIDGE_STATE_REPLICA_ENABLED=false` 关闭，或用 `MINDBRIDGE_STATE_FOLLOWER_DB_PATH`、`MINDBRIDGE_STATE_FOLLOWER_NODE_ID`、`MINDBRIDGE_STATE_REPLICA_POLL_MS`、`MINDBRIDGE_STATE_REPLICA_BATCH` 覆盖。`scripts/start_demo.sh` 默认让 Gateway 和多个 Counselor 共享同一个 master SQLite，以保证前端历史具备 read-after-write 语义；各 Counselor 仍有独立 follower SQLite 用于 replay 验证。
- 状态主键必须包含 `user_id + conversation_id + namespace_name + state_key`；任何读写、同步和恢复都不得只按 `conversation_id` 访问记忆。
- `namespace_name` 用于隔离 `chat_memory`、`consult_memory`、`risk_memory`、`run_artifact` 等状态，普通闲聊和高风险心理咨询不能混用同一 namespace。
- 写入状态时需要同事务更新 `state_records` 并追加 `state_changes`；从节点通过 `ReplicaWorker` 按 `change_id` 增量 replay，并用 `version` 做幂等保护。
- Gateway 前端历史 API 默认读取 master state store，不能读取独立历史表。若未来切到 follower 读，必须先校验 `replica_progress.last_applied_change_id` 已追上目标 change，否则等待或 fallback master。
- Counselor 暴露只读接口 `GET /api/state/replication/status`，用于查看 master/follower DB 路径、节点、change count、last applied、applied/skipped 和后台 worker 状态。
- 复制状态查询必须通过 `state_replication_status` 工具进入 `ToolRegistry`，返回结构化 `ToolResult`，不绕过 `PermissionChecker`、Hooks 和 Trace。

### 5.2 CloudStorage（FastDFS + MySQL + Redis）
- 云存储用于运行附件和产物，不替代 `DistributedStateStore` 的会话一致性职责。
- FastDFS 只保存文件字节；MySQL 保存持久元数据和引用计数；Redis 保存分片上传会话、已上传分片索引和 24 小时续传 TTL。
- MySQL schema 固化在 `mindbridge_harness/configs/cloud_storage_mysql.sql`，表名使用 `mb_` 前缀；项目内服务编排在 `infra/cloud_storage/docker-compose.yaml`，默认暴露 MySQL `3307`、Redis `6379`、FastDFS HTTP `80` 和 HTTPS `443`，不依赖 `AI_YunCunChu/docker` 运行。
- Redis key 约定为 `mb:chunk:{upload_id}`，Hash 字段包括 `user_id`、`conversation_id`、`run_id`、`filename`、`md5`、`size`、`chunk_count`、`uploaded`、`owner_node`，TTL 为 24 小时。
- 秒传先查 `mb_file_info.md5`；命中时只新增 `mb_user_file_list` 用户引用并增加 `ref_count`，不重复写 FastDFS。
- 文件对象进入 Agent 运行上下文时，必须把对象元数据写入 `run_artifact` namespace，使主从复制链路能审计“哪个用户、哪个会话、哪个 run 使用了哪个对象”。
- Gateway 必须提供真实 storage endpoints：兼容 `/api/md5`、`/api/upload`、`/api/chunk_init`、`/api/chunk_upload`、`/api/chunk_merge`，并提供 `/api/storage/*` 新别名。
- 下载接口默认返回 inline `data_base64`，保持 fake/cloud 后端和前端契约一致；如需直接暴露 FastDFS URL，可显式设置 `MINDBRIDGE_STORAGE_DOWNLOAD_MODE=redirect`。
- 上传成功后需要写入文件元数据、用户引用和 `mb_storage_change_tasks`；`StorageSyncWorker`/补偿逻辑负责把任务幂等写入 `DistributedStateStore` 的 `run_artifact` namespace。
- 默认开发/CI 后端使用本地 fake object store + SQLite metadata 跑通全流程；设置 `MINDBRIDGE_STORAGE_BACKEND=cloud` 和 `MINDBRIDGE_STATE_BACKEND=mysql` 后 Gateway 使用项目内 MySQL metadata/state、Redis upload session 和 FastDFS object store。真实环境用 `scripts/verify_cloud_storage_live.sh` 验证 MySQL 表写入、Redis chunk TTL、FastDFS file_id/url 和 run_artifact MySQL state。

### 5.3 Platform Session 控制面
- `mindbridge_platform` 是云原生 Agent Platform MVP 的控制面，不是新的业务 Agent，也不直接执行工具动作。
- 平台元数据保存在 `.mindbridge/platform/platform.sqlite`，默认由 `MINDBRIDGE_PLATFORM_DB_PATH` 覆盖；该库只保存 Workspace、AgentCore 和 AgentSession inventory，不替代 `DistributedStateStore`。
- Platform Session 绑定 `workspace_id + user_id + conversation_id + agent_core_id`，并可记录最近 `run_id`。MindBridge runtime 仍使用 `user_id + conversation_id + namespace_name + state_key` 作为状态隔离边界。
- 前端选择 Platform Session 后，普通 chat、streaming chat 和 `session/end` 使用该 session 的 `conversation_id`；生成 `run_id` 后通过 `/api/platform/sessions/{session_id}/attach-run` 绑定回控制面，供 Events/Artifacts tab 读取。
- `/api/platform/sessions/{session_id}/events` 只读取 run artifact，把 `trace.jsonl`、`boundary_trace.jsonl` 和 artifact 存在性映射为 Universal Event；它不得伪造 tool 成功、模型成功或风险评估成功。
- `/api/platform/sessions/{session_id}/artifacts` 只列出当前 session 绑定 run 下允许展示的 `task_state.json`、`trace.jsonl`、`report.json`、`boundary_trace.jsonl` 和 `observability_report.json`。
- `scripts/verify_platform_browser_smoke.sh` 启动本地 `mindbridge_platform` 和 demo frontend，通过 Playwright 创建 Workspace/Session、绑定测试 run，并验证 Platform tab 可见 Universal Events 与 artifacts。
- MVP 不开放任意 shell 和全量文件系统浏览；未来如加入 terminal/filesystem sidecar，仍必须通过受控 API、权限检查、审计事件和 workspace 级隔离。
- K8s base profile 默认关闭 eBPF 并不创建 privileged DaemonSet；`k8s/profiles/ebpf/` 只能作为显式 opt-in profile 使用。

### 6. BenchmarkRunner + HardnessScorer（评测约束）
- 用 hardness/quality 任务集做稳定评测。
- 评测维度包含多轮、RAG、风险、MCP 和协议合规。

### 6.1 Sandboxed Browser QA（OpenSandbox Playwright）
- `scripts/run_sandbox_browser_qa.sh` 提供非 K8s 的 OpenSandbox Playwright QA 入口，用隔离的
  Chromium/Playwright 环境访问 MindBridge demo 前端和 Gateway。
- 该能力参考 OpenSandbox `examples/playwright` 的隔离浏览器运行方式，但验证对象是 MindBridge
  心理咨询业务链路：普通咨询回复、高风险咨询回复、`risk_memory`/高风险评估证据、显式
  `session/end` 阶段性评估、Stored History、latest run artifact API 和 Observability API。
- Browser QA 不进入 Counselor/Evaluator 主链路，不新增 Agent，也不替代 benchmark；它是面向
  多服务前端回归的隔离验证层。后续如接入运行时工具，仍必须通过
  `ToolRegistry -> PermissionChecker -> HookExecutor -> TraceRecorder`。
- 默认 artifact 输出到 `.mindbridge/sandbox_qa/<run_id>/qa_result.json` 和
  `.mindbridge/sandbox_qa/<run_id>/mindbridge-browser-qa.png`，避免与 runtime run artifact
  目录 `.mindbridge/runs/<run_id>/` 的 `task_state.json` / `trace.jsonl` / `report.json`
  契约混用。
- 截图只是 QA 附件，不是验收目标；验收以 `qa_result.json` 中的业务检查为准，例如
  `consult_response_rendered`、`risk_response_rendered`、`risk_route_recorded`、
  `session_assessment_completed`、`stored_history_turns`、`run_artifact_contract`。
- `MINDBRIDGE_SANDBOX_QA_DRY_RUN=1` 只验证 MindBridge 侧 artifact/schema 和脚本布线，会写入
  `mindbridge-browser-qa.txt`，但不会创建 OpenSandbox sandbox 或启动 Chromium；真实浏览器隔离验证
  仍需安装 OpenSandbox SDK/runtime 后运行非 dry-run。
- `bash scripts/verify_sandbox_browser_qa.sh` 是该能力的独立验证入口；默认运行 unit/syntax/dry-run
  检查，设置 `MINDBRIDGE_SANDBOX_QA_LIVE=1` 后才执行真实 OpenSandbox 浏览器 sandbox。
- `bash scripts/verify_sandbox_browser_qa_dashscope_live.sh` 是真实 DashScope 供应商路径的安全封装：
  从当前环境或交互式隐藏 prompt 读取 `DASHSCOPE_API_KEY`，强制 remote model，启动
  `scripts/start_demo_dashscope.sh`，再执行 live OpenSandbox QA。该脚本只打印 key 是否已设置，不回显或保存 key。
- 本阶段不启用 Kubernetes；sandbox 访问本机服务时，脚本会优先检测 Docker bridge 网关
  `docker0`（常见为 `172.17.0.1`），否则回退到 `host.docker.internal`。可用
  `MINDBRIDGE_SANDBOX_FRONTEND_URL` 和 `MINDBRIDGE_SANDBOX_GATEWAY_URL` 覆盖。

### 7. MultimodalAnalyzer（多模态先验）
- 多模态能力必须通过 `ToolRegistry` 暴露为工具，不新增 `ToolAgent`。
- V1 工具 `analyze_multimodal_emotion` 接收 text/image，默认调用 DashScope/Qwen 多模态 API；MiMo 保留为回退 provider。
- 工具必须输出结构化 `ToolResult`，并由 C++ 侧重新计算融合分数与风险等级。
- provider 未配置或返回异常时，工具返回 disabled/low 风险，不阻断普通文本咨询。

### 8. Speech（语音输入与回复）
- `recognize_speech` 工具通过 DashScope `fun-asr-realtime` 把前端 16k wav 转成文字。
- ASR 只读取 `MINDBRIDGE_DASHSCOPE_ASR_API_KEY`、`MINDBRIDGE_DASHSCOPE_API_KEY` 或 `DASHSCOPE_API_KEY`；不要把 OpenAI-compatible/MiMo `MINDBRIDGE_MODEL_API_KEY` 当 DashScope WebSocket key 使用。
- ASR WebSocket 音频上传块大小和节奏由 `MINDBRIDGE_ASR_CHUNK_BYTES`、`MINDBRIDGE_ASR_CHUNK_DELAY_MS` 控制，避免短语音被过慢上传。
- 前端先展示 ASR 文字，再把文字和摄像头截图送入 Agent。
- `synthesize_speech` 工具默认通过 DashScope `cosyvoice-v3-flash` 合成语音；voice_id 只从环境变量读取，`cosyvoice-v3.5-*` 仅用于声音设计/复刻音色。若系统音色误配到 v3.5 模型，运行时会自动降到 v3-flash。
- 文本回复支持 `message/stream` SSE；TTS 支持 `/api/demo/tts/stream` 以回调方式边接收音频边播放，播放期间前端暂停 ASR 采样以降低回声。

## 心理健康场景的安全闭环

1. 用户输入进入 Gateway/Orchestrator。
2. Orchestrator 用轻量 `RiskPolicy` 判断是否需要 Evaluator，并按 `contextId` 记录最近会话 transcript。
3. 如果请求包含图片，Orchestrator 先调用 `analyze_multimodal_emotion` 生成风险先验；语音先由 Gateway ASR 转成文字。
4. 高风险请求先触发 Evaluator；Evaluator 通过 `mcp_email_alert` 执行外部预警。
5. Counselor 进入主咨询链路，使用 `assess_risk`、RAG、记忆和模型生成回复。
6. 用户显式结束会话时，Orchestrator 以 `session/end` 触发 Evaluator 基于最近 transcript 生成阶段性心理状态评估。
7. 阶段性评估完成后，Evaluator 无论风险高低都通过 `mcp_email_alert` 发送评估邮件；未配置 MCP endpoint 时返回未发送状态但不阻断请求。
8. Counselor 通过 `mcp_excel_write` 记录咨询结果；直接访问 Counselor 的高风险请求仍有邮件兜底。
9. 全流程事件写入 Trace，benchmark 定期验证是否退化。

## 常见误区

- 把所有规则塞进一个超长 prompt。
- 把 MCP 误当独立 Agent，而不是工具协议。
- 只看“回答流畅度”，不看风险识别和工具投递成功率。
- 改了架构但不更新 feature status 和验证脚本。
