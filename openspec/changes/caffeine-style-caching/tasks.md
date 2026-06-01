## 1. CaffeineCache 核心模板类

- [ ] 1.1 创建 `mcp/include/agent_rpc/mcp/rag/caffeine_cache.h` — 定义 `CacheConfig`、`CacheStats`、`CacheEntry`、`CaffeineCache<K,V>` 模板类接口
- [ ] 1.2 实现 `CaffeineCache` 基础操作 — `get()`、`put()`、`invalidate()`、`invalidate_all()`、`get_if_present()`，使用 LRU-K (K=2) 淘汰策略
- [ ] 1.3 实现 `expireAfterWrite` — 基于 `created_at` 时间戳的 TTL 过期检查
- [ ] 1.4 实现 `LoadingCache` 语义 — `get()` miss 时自动调用 `Loader` 函数，支持异常传播
- [ ] 1.5 实现 `refreshAfterWrite` — 后台刷新线程 + `std::condition_variable` + `std::queue`，返回旧值 + 异步刷新
- [ ] 1.6 实现 `CacheStats` 统计 — hits、misses、evictions、refreshes、size、`hitRate()`
- [ ] 1.7 实现环境变量配置 — `MINDBRIDGE_CACHE_ENABLED`、`MINDBRIDGE_CACHE_MAX_SIZE`、`MINDBRIDGE_CACHE_WRITE_TTL_SECONDS`、`MINDBRIDGE_CACHE_REFRESH_SECONDS`
- [ ] 1.8 编写单元测试 — 覆盖 LRU-K 淘汰、TTL 过期、refreshAfterWrite、并发安全、Loader 异常、disabled bypass

## 2. DistributedStateStore L1 缓存集成

- [ ] 2.1 在 `distributed_state_store.hpp` 的 `Impl` 中添加 `CaffeineCache` 成员 — state record cache + conversation history cache
- [ ] 2.2 修改 `get_record()` — 查询前先检查 L1 缓存，miss 时查 DB 并回填缓存，not-found 结果缓存短 TTL
- [ ] 2.3 修改 `conversation_history()` — 使用 `refreshAfterWrite` 缓存，热点对话返回旧值 + 异步刷新
- [ ] 2.4 修改 `upsert_record()` — 写 DB 成功后 write-through 更新缓存，conversation namespace 写入时失效 history cache
- [ ] 2.5 确保 `upsert_record()` 内部 version lookup 绕过缓存直接查 DB
- [ ] 2.6 添加状态缓存环境变量 — `MINDBRIDGE_STATE_CACHE_ENABLED`、`MINDBRIDGE_STATE_CACHE_MAX_SIZE`
- [ ] 2.7 编写集成测试 — 验证缓存命中/未命中行为、write-through 更新、history cache 刷新

## 3. SessionStore 缓存集成

- [ ] 3.1 在 `session_store.hpp` 中添加 `CaffeineCache` 成员
- [ ] 3.2 修改 `load()` — 查询前先检查缓存，miss 时读文件并回填
- [ ] 3.3 修改 `save()` — 写文件后 write-through 更新缓存
- [ ] 3.4 配置 `expireAfterAccess` — 不活跃 session 自动淘汰
- [ ] 3.5 添加 session 缓存环境变量 — `MINDBRIDGE_SESSION_CACHE_ENABLED`、`MINDBRIDGE_SESSION_CACHE_MAX_SIZE`
- [ ] 3.6 编写单元测试 — 验证 load 缓存命中、save 更新缓存、expireAfterAccess 淘汰

## 4. PromptPrefix Cache 集成

- [ ] 4.1 在 `agent_loop.hpp` 中添加 prompt cache 成员 — key 为 `PromptPrefix::hash`
- [ ] 4.2 修改 `AgentLoop::run()` / `run_stream()` — 构建 prompt 前查缓存，命中则跳过 `PromptPolicy::build()` + `ContextManager::build()`
- [ ] 4.3 实现 tool_signature 变更时的缓存失效
- [ ] 4.4 添加 prompt 缓存环境变量 — `MINDBRIDGE_PROMPT_CACHE_ENABLED`、`MINDBRIDGE_PROMPT_CACHE_TTL_SECONDS`
- [ ] 4.5 更新 `feature_status.json` — 将 `pico_prompt_prefix_runtime_loop` 的 `next_step` 更新为 prompt cache 已实现

## 5. 集成验证

- [ ] 5.1 编译验证 — `cmake --build build --target mindbridge_harness -j2` 无编译错误
- [ ] 5.2 运行现有测试 — `ctest --test-dir build --output-on-failure` 确保无回归
- [ ] 5.3 端到端验证 — 启动 gateway + counselor，发送多轮对话，验证缓存命中率日志
- [ ] 5.4 性能对比 — 对比有/无缓存的 get_record 延迟和 SQLite 查询次数
