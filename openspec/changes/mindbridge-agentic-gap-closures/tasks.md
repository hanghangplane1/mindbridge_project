## 1. RAG 激活（前置依赖）

- [ ] 1.1 新增 `tools/knowledge_ingest/main.cpp`：CLI 工具，接受 `--input-dir`、`--collection`、`--chroma-url`、`--embedding-model`、`--dry-run` 参数
- [ ] 1.2 实现文档读取：支持 .txt / .md 文件，递归扫描目录
- [ ] 1.3 实现文本切片：按 ~500 token 切片，50 token overlap，使用 TokenCounter（如 tiktoken-cpp 未就绪则用字符数估算）
- [ ] 1.4 实现 embedding 生成：调用 ModelClient::embedding() 为每个 chunk 生成向量
- [ ] 1.5 实现 ChromaDB upsert：POST 到 `/api/v1/collections/{name}/upsert`，批量提交 chunks + embeddings + metadata（source_file、chunk_index、content_hash）
- [ ] 1.6 实现增量更新：对比 content_hash，仅重新入库变更的文档
- [ ] 1.7 修改 `scripts/start_demo.sh`：启动前检查 ChromaDB 可达性，不可达则提示用户启动；导出 `MINDBRIDGE_CHROMA_URL` 和 `MINDBRIDGE_CHROMA_COLLECTION` 环境变量
- [ ] 1.8 修改 `counselor_main.cpp`：启动时做 test embedding 验证链路，失败时 log warning 并降级为 disabled 模式
- [ ] 1.9 修改 `RetrieveKnowledgeTool::execute()`：disabled 模式时 log 明确的 warning（而非静默返回空）
- [ ] 1.10 将 rag_contexts_count 和 rag_retrieval_ms 写入 trace 的 run 事件

## 2. 基础设施（tiktoken-cpp 集成）

- [ ] 2.1 将 tiktoken-cpp 作为子模块或 FetchContent 依赖加入 CMakeLists.txt
- [ ] 2.2 实现 `TokenCounter` 工具类：封装 tiktoken-cpp 的 cl100k_base 编码，提供 `count_tokens(string) → int` 接口
- [ ] 2.3 实现字符数回退模式：当 tiktoken-cpp 初始化失败时，按 1 token ≈ 2 CJK / 4 ASCII + 1.2x 安全系数估算
- [ ] 2.4 为 TokenCounter 编写单元测试（中英文混合、空字符串、超长文本）

## 3. Token Budget 控制

- [ ] 3.1 新增 `include/mindbridge/harness/token_budget.hpp`：定义 TokenBudgetController 类，含 total_budget、warning/critical/hard_limit 阈值、consumed 计数器
- [ ] 3.2 实现 `record_usage(prompt_tokens, completion_tokens, tool_output_tokens)` 方法，累计消耗并返回当前 budget_level（GREEN/YELLOW/RED/BREAKER）
- [ ] 3.3 实现 `check_action_allowed(action_type)` 方法：RED 级别拒绝非关键工具，BREAKER 级别拒绝所有操作
- [ ] 3.4 修改 AgentLoop::run()：在每步 generate() 和 invoke_tool() 后调用 TokenBudgetController::record_usage()
- [ ] 3.5 修改 AgentLoop::run()：在每步开始时检查 budget_level，YELLOW 按 intent 区分降级（CHAT 完整压缩、CONSULT 仅截断历史、RISK 不压缩只记日志），RED 跳过可选工具，BREAKER 终止 run
- [ ] 3.6 扩展 RunInput：新增可选字段 `token_budget_limit`（默认 8000）
- [ ] 3.7 扩展 RunResult：新增字段 `token_budget_used`、`token_budget_limit`、`budget_level_reached`
- [ ] 3.8 将 budget 事件（budget_warning/budget_critical/budget_exceeded）写入 trace.jsonl
- [ ] 3.9 为 TokenBudgetController 编写单元测试（各级别触发、边界条件）

## 4. 模型路由

- [ ] 4.1 新增 `include/mindbridge/model/model_router.hpp`：定义 ModelRouter 类，持有 model profile 表和 ModelClient 实例池
- [ ] 4.2 定义 ModelProfile 结构体：intent、primary_model_id、fallback_model_id、max_tokens、temperature
- [ ] 4.3 实现模型配置加载：从 JSON 配置文件读取 profile 表，支持默认映射（CHAT→轻量、CONSULT→标准、RISK→强模型）
- [ ] 4.4 实现 `select_model(intent, complexity_score) → ModelClient*` 方法：按 intent 查表选择模型
- [ ] 4.5 实现 fallback 逻辑：primary 模型失败时自动切换 fallback 并记录 trace 事件
- [ ] 4.6 修改 Orchestrator/Counselor：在调用 AgentLoop 前通过 ModelRouter 选择 ModelClient
- [ ] 4.7 将 model_selected 和 model_fallback 事件写入 trace.jsonl
- [ ] 4.8 支持 SIGHUP 信号触发热重载模型配置
- [ ] 4.9 为 ModelRouter 编写单元测试（intent 映射、fallback、配置重载）

## 5. MCP Server 配额

- [ ] 5.1 新增 `include/mindbridge/harness/mcp_quota.hpp`：定义 McpQuotaManager 类，含令牌桶和滑动窗口计数器
- [ ] 5.2 实现 TokenBucket：`rate_limit`（每秒令牌数）、`burst_size`（桶容量）、`try_consume() → bool` 方法
- [ ] 5.3 实现 SlidingWindowBudget：`daily_budget`（24h token 上限）、`record_usage(tokens)`、`is_exceeded() → bool`
- [ ] 5.4 扩展 ToolRegistry 的 MCP Server 配置：新增 `rate_limit`、`burst_size`、`daily_budget` 字段
- [ ] 5.5 修改 ToolRegistry::invoke_tool()：在调用 MCP 工具前检查令牌桶，超限则排队等待最多 5 秒
- [ ] 5.6 修改 ToolRegistry::invoke_tool()：在调用 MCP 工具前检查日预算，超限则拒绝
- [ ] 5.7 将 mcp_rate_limited 和 mcp_budget_exceeded 事件写入 trace.jsonl
- [ ] 5.8 支持配额配置热重载
- [ ] 5.9 为 McpQuotaManager 编写单元测试（令牌桶突发、滑动窗口过期、并发安全）

## 6. 记忆评分淘汰

- [ ] 6.1 新增 `include/mindbridge/runtime/memory_scorer.hpp`：定义 MemoryScorer 类，含 λ、importance 权重等配置参数
- [ ] 6.2 实现 `recency(age_hours) → float`：exp(-λ × age_hours)，λ 默认 0.05
- [ ] 6.3 实现 `relevance(query_embedding, memory_embedding) → float`：余弦相似度，复用已有 embedding 接口
- [ ] 6.4 实现 `importance(metadata) → float`：RISK=1.5、情绪标记=1.3、普通=1.0
- [ ] 6.5 实现 `score(memory, query_embedding) → float`：三因子乘积
- [ ] 6.6 修改 ConversationMemory：新增 max_entries 配置（默认 50），超限时调用 MemoryScorer 评分并淘汰低分条目
- [ ] 6.7 实现淘汰保护：risk_level=RISK 的记忆不被淘汰（跳过选次低分）
- [ ] 6.8 实现 embedding 缓存：为每条记忆缓存 embedding，仅新记忆调用 ModelClient::embedding()
- [ ] 6.9 实现评分触发：新消息到达时重算 relevance，定时器每 30 分钟重算 recency
- [ ] 6.10 实现 embedding 失败回退：embedding 不可用时降级为 recency × importance 双因子评分
- [ ] 6.11 将 memory_evicted 事件写入 trace.jsonl（含被淘汰条目的 score）
- [ ] 6.12 为 MemoryScorer 编写单元测试（recency 衰减曲线、importance 加权、淘汰顺序、embedding 回退）

## 7. 轨迹分析

- [ ] 7.1 新增 `include/mindbridge/harness/trajectory_analyzer.hpp`：定义 TrajectoryAnalyzer 类
- [ ] 7.2 实现循环检测：维护最近 N 个 tool_call 的 hash，连续重复超阈值（默认 3）则标记 loop_detected
- [ ] 7.3 实现效率评分：min_tool_calls（按 intent 查配置表）/ actual_tool_calls
- [ ] 7.4 实现异常检测：高频失败率（>50%）、未使用工具输出、超步数（>max_tool_steps × 1.5）
- [ ] 7.5 修改 AgentLoop::run()：在 run_finished 前调用 TrajectoryAnalyzer 分析本次轨迹
- [ ] 7.6 循环检测触发时，AgentLoop 强制跳出循环（选择不同 action）
- [ ] 7.7 将 trajectory_efficiency_score、trajectory_loop_detected、trajectory_anomalies 写入 run_finished trace 事件
- [ ] 7.8 将分析参数（loop_threshold、efficiency_estimates、anomaly_thresholds）外置到配置文件
- [ ] 7.9 为 TrajectoryAnalyzer 编写单元测试（循环检测、效率计算、异常标记）

## 8. 集成测试与文档

- [ ] 8.1 编写端到端集成测试：模拟 RISK 意图 run 触发 Yellow 级别时上下文不被压缩，CHAT 意图 run 触发 Yellow 级别时上下文被压缩
- [ ] 8.2 编写集成测试：验证 ModelRouter 在 primary 失败时自动 fallback
- [ ] 8.3 编写集成测试：验证 MCP 配额在高频调用下的限流和排队行为
- [ ] 8.4 编写集成测试：验证 MemoryScorer 在记忆超限时正确淘汰低分条目并保留 RISK 条目
- [ ] 8.5 编写集成测试：验证 RAG 端到端链路（入库文档 → retrieve_knowledge 返回结果 → 注入 prompt）
- [ ] 8.6 更新 docs/architecture.md：补充 6 个新组件的架构说明（含 RAG 激活）
- [ ] 8.7 更新 docs/harness_engineering.md：补充 TokenBudget、ModelRouter、McpQuotaManager、RAG 的工程约束
