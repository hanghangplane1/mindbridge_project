## Why

MindBridge 的核心存储层（DistributedStateStore、CloudStorageService、SessionStore）每次读操作都直接穿透到 SQLite/MySQL，没有任何进程内缓存。在高并发心理咨询场景下，热点数据（活跃对话历史、state records、session 数据）反复打 DB，造成不必要的 I/O 开销和延迟。唯一的缓存是 EmbeddingCache（简单 LRU），仅覆盖 RAG embedding 向量，无法复用到其他存储层。

Caffeine（Java）的三大核心模式——Window TinyLFU 淘汰、refreshAfterWrite 异步刷新防击穿、LoadingCache 自动加载——完全可以用 C++ 重实现，并作为通用缓存基础设施集成到 MindBridge 的存储层中，解决性能和击穿两个问题。

## What Changes

- 新增通用 `CaffeineCache<K,V>` C++ 模板类，支持 TinyLFU 淘汰、expireAfterWrite、refreshAfterWrite、LoadingCache 语义、命中率统计
- 给 `DistributedStateStore` 加 L1 进程内读缓存，覆盖 `get_record` 和 `conversation_history` 两个热路径
- 给 `SessionStore` 加热路径缓存，活跃 session 不必每次全量读文件
- 实现 PromptPrefix cache，利用已有 `PromptPrefix::hash` 作为缓存 key
- 可选：给 `ToolRegistry` 加确定性工具结果缓存（calculator、weather 等）
- 更新 `feature_status.json` 中 prompt_prefix_cache 的状态

## Capabilities

### New Capabilities
- `caffeine-cache-core`: 通用 CaffeineCache<K,V> C++ 模板类，提供 LRU/TinyLFU 淘汰、TTL 过期、refreshAfterWrite 异步刷新、LoadingCache 自动加载、命中率统计等核心缓存能力
- `state-store-read-cache`: 给 DistributedStateStore 加 L1 进程内读缓存，覆盖 get_record 和 conversation_history 热路径，支持 refreshAfterWrite 防击穿
- `session-store-cache`: 给 SessionStore 加热路径缓存，活跃 session 内存化，expireAfterAccess 自动淘汰
- `prompt-prefix-cache`: 利用 PromptPrefix::hash 作为 key 缓存构建好的 prompt，避免重复构建

### Modified Capabilities
<!-- 无现有 spec 需要修改 -->

## Impact

- **代码**: `mcp/include/agent_rpc/mcp/rag/caffeine_cache.h`（新文件）、`mindbridge_harness/src/state/distributed_state_store.cpp`、`mindbridge_harness/src/runtime/session_store.cpp`、`mindbridge_harness/src/runtime/prompt_prefix.cpp`、`mindbridge_harness/src/harness/agent_loop.cpp`
- **依赖**: 无新外部依赖，基于现有 C++ 标准库实现
- **配置**: 新增环境变量 `MINDBRIDGE_CACHE_MAX_SIZE`、`MINDBRIDGE_CACHE_TTL_SECONDS`、`MINDBRIDGE_CACHE_REFRESH_SECONDS`
- **线程安全**: CaffeineCache 自带 mutex 保护，解决 DistributedStateStore 的 SQLite 并发访问问题
- **存储**: 缓存为 L1 层，不改变现有 SQLite/MySQL/Redis 存储后端
