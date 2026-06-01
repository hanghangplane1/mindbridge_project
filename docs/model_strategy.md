# 模型策略（MindBridge）

## 结论

MindBridge 工程主链路采用：

- 基础模型（Ollama 或 OpenAI-compatible）
- PromptPolicy 约束
- RAG 检索增强
- Evaluator 风险兜底
- MCP 工具动作闭环

LoRA 保留为实验对照，不作为生产主依赖。

## 为什么主链路不用 LoRA

- 场景安全边界比风格拟合更重要。
- LoRA 训练数据分布与真实线上输入常有偏差。
- 合并/量化后行为不稳定，运维复杂度上升。
- Prompt + RAG + 风险策略更可控、更容易迭代。

## 模型接入方式

统一使用 `ModelClient` 抽象，现支持：

- `OllamaModelClient`
- `OpenAICompatibleModelClient`
- `DashScopeModelClient`（百炼原生 HTTP endpoint）

环境变量建议：

- `MINDBRIDGE_MODEL_PROVIDER=auto|ollama|openai_compatible|dashscope_native`
- `MINDBRIDGE_REQUIRE_REMOTE_MODEL=1`
- `MINDBRIDGE_MODEL_BASE_URL`
- `MINDBRIDGE_MODEL_API_KEY`
- `MINDBRIDGE_MODEL_NAME`
- `MINDBRIDGE_ORCHESTRATOR_URLS`
- `MINDBRIDGE_COUNSELOR_URLS`
- `MINDBRIDGE_EVALUATOR_URLS`
- `MINDBRIDGE_HEALTH_INTERVAL_MS`
- `MINDBRIDGE_HEALTH_FAIL_THRESHOLD`
- `MINDBRIDGE_HEALTH_RECOVER_THRESHOLD`
- `MINDBRIDGE_CIRCUIT_OPEN_MS`
- `MINDBRIDGE_HEALTH_TIMEOUT_SEC`
- `MINDBRIDGE_NET_BIND_ADDRESS`
- `MINDBRIDGE_NET_IO_THREADS`
- `MINDBRIDGE_NET_MAX_REQUEST_BYTES`
- `MINDBRIDGE_NET_REQUEST_TIMEOUT_SEC`
- `MINDBRIDGE_NET_SERVER_NAME`
- `MINDBRIDGE_NET_TLS_VERIFY`
- `MINDBRIDGE_NET_CA_FILE`
- `MINDBRIDGE_EMBEDDING_MODEL`
- `MINDBRIDGE_CHAT_MAX_TOKENS`
- `MINDBRIDGE_EVALUATOR_MAX_TOKENS`
- `MINDBRIDGE_SESSION_SUMMARY_MAX_TOKENS`
- `MINDBRIDGE_QWEN_CHAT_ENABLE_THINKING`
- `MINDBRIDGE_QWEN_EVALUATOR_ENABLE_THINKING`
- `MINDBRIDGE_MULTIMODAL_PROVIDER=dashscope|mimo`
- `MINDBRIDGE_DASHSCOPE_API_KEY` 或 `DASHSCOPE_API_KEY`
- `MINDBRIDGE_DASHSCOPE_NATIVE_BASE_URL`
- `MINDBRIDGE_DASHSCOPE_BASE_URL`
- `MINDBRIDGE_DASHSCOPE_MULTIMODAL_MODEL`
- `MINDBRIDGE_ASR_PROVIDER=dashscope`
- `MINDBRIDGE_DASHSCOPE_ASR_MODEL`
- `MINDBRIDGE_MIMO_BASE_URL`
- `MINDBRIDGE_MIMO_MODEL`
- `MINDBRIDGE_MIMO_API_KEY`
- `MINDBRIDGE_TTS_PROVIDER=dashscope|mimo`
- `MINDBRIDGE_DASHSCOPE_TTS_MODEL`
- `MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID`
- `MINDBRIDGE_DASHSCOPE_TTS_FORMAT`
- `MINDBRIDGE_MIMO_TTS_BASE_URL`
- `MINDBRIDGE_MIMO_TTS_MODEL`
- `MINDBRIDGE_MIMO_TTS_API_KEY`
- `MINDBRIDGE_MIMO_TTS_VOICE`

默认 provider 为 `auto`。当只配置 DashScope key 且没有显式 `MINDBRIDGE_MODEL_BASE_URL`
时，运行时走百炼原生 `DashScopeModelClient`，纯文本请求使用
`/api/v1/services/aigc/text-generation/generation`；`qwen3.6-*`、`qwen3.5-*` 和
`*-vl*` 模型使用 `/api/v1/services/aigc/multimodal-generation/generation`
（`qwen3.6-max-preview` 仍走文本接口），不拼 OpenAI-compatible `/v1/chat/completions`。
显式配置 `MINDBRIDGE_MODEL_PROVIDER=openai_compatible` 时仍可使用兼容模式；OpenAI-compatible
Base URL 可以带 `/v1`，运行时会避免重复拼出 `/v1/v1/chat/completions`。
模型请求组装前会对 prompt、历史上下文、用户输入和模型返回文本做 UTF-8 清洗；
`ContextManager` 的预算裁剪按 UTF-8 字符边界截断，避免中文多轮历史在 JSON
序列化时触发 `invalid UTF-8 byte`。

MiMo 凭证需要匹配 base URL：

- `sk-...` 按量付费 key 使用 `https://api.xiaomimimo.com/v1`
- `tp-...` Token Plan key 使用 `https://token-plan-cn.xiaomimimo.com/v1`

`scripts/start_demo.sh` 会在只提供 `tp-...` key 时自动选择 Token Plan base URL。
如果 Token Plan 网关返回 500/502，`OpenAICompatibleModelClient` 会对可重试错误做
短间隔重试；持续失败时会把供应商错误透传到 run artifact，便于排查。

如果不希望本地部署 Ollama，使用 `scripts/start_demo_dashscope.sh` 或
`scripts/start_demo_openai.sh`。这两个脚本会设置 `MINDBRIDGE_REQUIRE_REMOTE_MODEL=1`，
因此远程 API 配置缺失时会直接失败，不会 fallback 到本地 Ollama。DashScope 启动脚本
还会在启动服务前用 `MINDBRIDGE_PREFLIGHT_MODEL`（默认 `qwen-plus`）做一次 API key
preflight；如果 key 被百炼返回 401，会停止启动并提示换新 key，避免前端刷新后反复看到
运行时 `InvalidApiKey`。

## 运行时策略

1. Orchestrator 先按 `contextId` 对 Counselor/Evaluator 多副本做一致性哈希路由。
2. `CONSULT/RISK` 分支才走 RAG，`CHAT` 走轻链路。
3. `CONSULT` prompt 会把命中的 RAG 内容标为“MindBridge 知识库检索结果”，要求模型优先使用相关内容；若无命中则退回通用心理支持知识。咨询回复默认给出 220 到 360 个中文字，包含 3 到 4 条可执行下一步、观察信号和求助边界；用户表达感谢、好转或准备尝试时仍要承接前文困扰并给出巩固练习和复发预案。
4. 生成后 `ResponseValidator` 会检查有害内容、高风险热线、RAG 关键项引用，以及“复读用户最新发言”这类无效回复；失败时 Counselor 会重试一次并带上纠错说明。
5. 生成后按风险策略决定是否触发即时 MCP 邮件。
6. `session/end` 无论风险高低都会调用 Evaluator 生成阶段性总结，并通过 MCP 邮件投递报告。
7. 所有动作进入 trace 与 benchmark。

## 质量评测建议

- 不只看主观“像不像人说话”。
- 至少跟踪以下指标：
  - 请求成功率
  - RAG 命中率
  - 风险识别准确率
  - MCP 投递成功率
  - JSON/SSE 协议合规率

## 安全要求

- API Key 只通过环境变量传入。
- 禁止将 key 写入源码、脚本、文档、提交信息。
- 验证入口统一走 `scripts/verify_mindbridge.sh`。
- eBPF BoundaryObservability 默认只记录模型请求元信息和长度摘要，不记录完整 prompt、用户咨询正文、API key 或 HTTPS 明文 payload；`MINDBRIDGE_EBPF_CAPTURE_LLM` 默认必须保持 `off`。调试关联时间线时可显式设置 `MINDBRIDGE_OBSERVABILITY_CAPTURE_TEXT=1`，此时只保存由 `MINDBRIDGE_OBSERVABILITY_TEXT_LIMIT` 限制的 prompt/response/user message excerpt，不能写入 API key、Authorization header 或其他 secret。

## 登录与隔离配置

用户登录由 Gateway 处理，不进入 `ModelClient`。启用 `MINDBRIDGE_AUTH_REQUIRED=true` 后，
用户使用 AK/SK 调用 `/api/auth/login`，Gateway 签发短期 token，并在转发到
Orchestrator/Counselor 前注入 `metadata.user_id` 和 scoped `contextId`。

相关环境变量：

- `MINDBRIDGE_AUTH_REQUIRED=false|true`
- `MINDBRIDGE_AUTH_DB_PATH=.mindbridge/mindbridge.db`
- `MINDBRIDGE_AUTH_BOOTSTRAP_USERS`：运行时注入初始用户 JSON，禁止写入仓库文件。
- `MINDBRIDGE_AUTH_SESSION_TTL_SEC`：token 默认 8 小时过期。

Run artifacts 继续写入 `.mindbridge/runs/<run_id>/`，Gateway 通过 SQLite `run_ownership`
表限制查询范围，避免不同用户看到彼此的对话结果。

## DashScope/Qwen 配置

主链路推荐直接使用阿里云百炼原生 API：

```bash
export DASHSCOPE_API_KEY=...
export MINDBRIDGE_MODEL_PROVIDER=dashscope_native
export MINDBRIDGE_MODEL_NAME=qwen-plus
```

如需 OpenAI-compatible 模式，可显式配置：

```bash
export DASHSCOPE_API_KEY=...
export MINDBRIDGE_MODEL_PROVIDER=openai_compatible
export MINDBRIDGE_MODEL_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1
export MINDBRIDGE_MODEL_NAME=qwen-plus
export MINDBRIDGE_QWEN_ENABLE_THINKING=true
```

`OpenAICompatibleModelClient` 会兼容读取 `DASHSCOPE_API_KEY`。普通咨询默认不启用
Qwen thinking，以降低首 token 延迟；需要时可设置
`MINDBRIDGE_QWEN_CHAT_ENABLE_THINKING=true`。Evaluator 可通过
`MINDBRIDGE_QWEN_EVALUATOR_ENABLE_THINKING=true` 或兼容旧变量
`MINDBRIDGE_QWEN_ENABLE_THINKING=true` 开启 thinking。运行时只消费模型 `content`，
不把 `reasoning_content` 作为用户可见回复或 trace 内容。

## 多模态模型策略

V1 多模态默认使用 DashScope/Qwen 作为可替换的 `MultimodalAnalyzer` 实现。
前端先把语音通过 ASR 转成文字，再发送文字和摄像头截图；Orchestrator 通过
`analyze_multimodal_emotion` 工具调用 Qwen chat completions。图片以
`data:{MIME_TYPE};base64,...` 形式传入。

默认配置：

- `MINDBRIDGE_MULTIMODAL_PROVIDER=dashscope`
- `MINDBRIDGE_DASHSCOPE_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1`
- `MINDBRIDGE_DASHSCOPE_MULTIMODAL_MODEL=qwen3.6-plus`
- `MINDBRIDGE_DASHSCOPE_API_KEY`，未设置时兼容读取 `DASHSCOPE_API_KEY` 或 `MINDBRIDGE_MODEL_API_KEY`

MiMo 实现仍保留为回退 provider。后续如接 Whisper + MediaPipe，只需新增另一个
`MultimodalAnalyzer` 实现，不要改变 Orchestrator/Counselor/Evaluator 的主协议。

## 语音识别策略

Demo 视频通话模式下，前端会把最近一段麦克风音频转成 16kHz wav 后调用
Gateway `/api/demo/asr`。Gateway 通过 `recognize_speech` 工具调用
DashScope WebSocket ASR，默认模型 `fun-asr-realtime`。识别出的文字会先展示在
前端用户气泡里，再与摄像头截图一起发送给 Agent。

默认配置：

- `MINDBRIDGE_ASR_PROVIDER=dashscope`
- `MINDBRIDGE_DASHSCOPE_ASR_API_KEY`（或 `MINDBRIDGE_DASHSCOPE_API_KEY` / `DASHSCOPE_API_KEY`）
- `MINDBRIDGE_DASHSCOPE_ASR_MODEL=fun-asr-realtime`
- `MINDBRIDGE_DASHSCOPE_ASR_WS_URL=wss://dashscope.aliyuncs.com/api-ws/v1/inference`
- `MINDBRIDGE_ASR_CHUNK_BYTES=6400`
- `MINDBRIDGE_ASR_CHUNK_DELAY_MS=100`

`MINDBRIDGE_MODEL_API_KEY` 不再作为 ASR 的隐式 fallback。非 DashScope 模型 key
不能用于 DashScope WebSocket ASR；普通 `start_demo.sh` 默认禁用 ASR，避免出现
WebSocket handshake declined。需要语音识别时，显式设置 `MINDBRIDGE_ASR_PROVIDER=dashscope`
并提供 DashScope ASR-capable key，或使用 `scripts/start_demo_dashscope.sh`。

## 语音回复策略

Demo 的伪视频通话模式下，助手文本回复后由 Gateway 的
`/api/demo/tts` 调用 `synthesize_speech` 工具合成语音。默认 provider 为
DashScope CosyVoice；TTS endpoint、voice、format 都通过环境变量配置，避免把
provider 细节写死在前端。默认模型为 `cosyvoice-v3-flash`，适配 `longanhuan`
等系统音色；`cosyvoice-v3.5-*` 无系统音色，仅用于声音设计/复刻音色。若检测到
系统音色误配到 v3.5 模型，运行时会自动降到 `cosyvoice-v3-flash`。

低延迟路径使用 `/api/demo/tts/stream`，后端通过 C++ `SpeechSynthesisCallback`
把 DashScope WebSocket 二进制音频块转成 SSE `audio` 事件，前端用 Web Audio
排队播放 PCM，实现边生成边播放。连续失败达到
`MINDBRIDGE_TTS_AUTO_DISABLE_THRESHOLD` 后会自动禁用 TTS，保留纯文本对话。

默认配置：

- `MINDBRIDGE_TTS_PROVIDER=dashscope`
- `MINDBRIDGE_DASHSCOPE_TTS_MODEL=cosyvoice-v3-flash`
- `MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID=<voice_id>`（未设置时流式/同步 TTS 会 fallback 到 `longanyang`）
- `MINDBRIDGE_DASHSCOPE_TTS_FORMAT=mp3`
- `MINDBRIDGE_DASHSCOPE_TTS_WS_URL=wss://dashscope.aliyuncs.com/api-ws/v1/inference`
- `MINDBRIDGE_TTS_AUTO_DISABLE_THRESHOLD=5`
- `MINDBRIDGE_TTS_CREATE_RETRIES=3`

MiMo TTS 实现仍保留为 provider 回退。
