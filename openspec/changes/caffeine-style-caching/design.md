## Context

MindBridge 的存储层当前架构：

- **DistributedStateStore**: 核心 KV 状态存储，SQLite（默认）或 MySQL 后端，每次 `get_record`/`conversation_history` 直接打 DB，无任何缓存。`upsert_record` 时额外一次 `get_record` 用于 version bump。SQLite 句柄无 mutex 保护。
- **CloudStorageService**: 文件元数据存储，本地模式用 SQLite+文件系统，云模式用 MySQL+Redis+FastDFS。MD5 内容去重已有，但元数据查询无缓存。
- **SessionStore**: 文件系统 JSON 文件，每次 `load` 全量读+解析，无缓存。
- **EmbeddingCache**: 唯一的缓存实现，简单 LRU（list + unordered_map + mutex），max_size=1000，ttl=3600s，线程安全。
- **PromptPrefix**: 无状态工具，已有 `hash` 字段可用作缓存 key，但未接入缓存。
- **ConversationMemory**: 纯内存 per-session 工作区，已有 knowledge summary map + eviction 逻辑。

现有缓存基础设施：EmbeddingCache 的 LRU 实现（~170 行 C++）可作为模板参考。Redis 仅用于云模式的 chunk upload session，未用于通用缓存。

## Goals / Non-Goals

**Goals:**
- 提供通用 `CaffeineCache<K,V>` C++ 模板类，支持 Caffeine 的核心模式
- 给 DistributedStateStore 加 L1 进程内读缓存，覆盖 get_record 和 conversation_history
- 给 SessionStore 加热路径缓存
- 实现 PromptPrefix cache
- 所有缓存组件线程安全
- 通过环境变量可配置、可禁用
- 零新外部依赖

**Non-Goals:**
- 不实现完整的 Window TinyLFU 算法（复杂度过高，LRU-K 或 2Q 足够）
- 不替换现有 EmbeddingCache（它工作正常，仅做接口对齐）
- 不改变 SQLite/MySQL/Redis 存储后端
- 不实现分布式缓存（进程内 L1 足够，Redis L2 是未来工作）
- 不实现 maximumWeight 按内存淘汰（需要 allocator hook，复杂度高）
- 不给 CloudStorageService 加缓存（它的元数据查询不是热路径）

## Decisions

### Decision 1: 淘汰策略 — LRU-K 而非 TinyLFU

**选择**: 使用 LRU-K（K=2，记录最近两次访问时间间隔）替代 Window TinyLFU。

**理由**: TinyLFU 需要 Count-Min Sketch + Doorkeeper 频率估计 + 频率- recency 混合比较，实现复杂度高（~800 行），且对 C++ 内存管理不友好。LRU-K 用两个时间戳（`last_access` 和 `prev_access`）即可区分扫描型访问和热点访问，实现简单（~50 行额外代码），命中率接近 TinyLFU。

**替代方案**:
- 纯 LRU: 最简单，但对扫描型访问不友好（EmbeddingCache 当前方案）
- TinyLFU: 命中率最优，但实现复杂，且需要额外的 sketch 数据结构内存
- 2Q: 类似 LRU-K，但用两个队列而非时间戳，实现略复杂

### Decision 2: refreshAfterWrite 用后台线程 + 条件变量

**选择**: 用 `std::thread` + `std::condition_variable` + `std::queue` 实现后台刷新。

**理由**: Caffeine Java 用 ForkJoinPool 异步刷新。C++ 没有标准线程池，但 `std::thread` + `condition_variable` 足够轻量。一个后台刷新线程处理所有 pending 的 refresh 请求，通过 `std::future` 将旧值返回给调用方。

**替代方案**:
- Boost.Asio `io_context::post()`: 项目已有 Boost 依赖，但引入 io_context 到缓存层会增加耦合
- `std::async`: 每次 refresh 启动一个 async，无法控制并发数
- 调用方同步刷新: 阻塞请求，违背 refreshAfterWrite 的"返回旧值"语义

### Decision 3: 缓存 key 设计 — 组合键的字符串序列化

**选择**: 将复合 key（如 DistributedStateStore 的 4-tuple）序列化为 `":"` 分隔的字符串。

**理由**: `std::unordered_map<std::string, ...>` 已在 EmbeddingCache 中验证可行。字符串序列化简单、可调试、hash 函数成熟（`std::hash<std::string>`）。DistributedStateStore 的 key 是 `(user_id, conversation_id, namespace, key)`，序列化为 `"uid:conv_ns:key"` 即可。

**替代方案**:
- 自定义 struct + hash: 需要实现 `operator==` 和 `std::hash` 特化，代码量更大
- `std::tuple` 作为 key: 需要 `std::hash<std::tuple>` 特化，不如字符串直观
- 用 `int64_t` hash 作为 key: hash 碰撞风险，调试不直观

### Decision 4: 线程安全 — 细粒度 mutex 而非全局锁

**选择**: CaffeineCache 内部用一个 `std::mutex` 保护所有状态（和 EmbeddingCache 一致），但在 DistributedStateStore 层面，缓存操作在 SQLite 操作之前/之后，不嵌套锁。

**理由**: EmbeddingCache 已验证单 mutex 方案在 max_size=1000 时性能足够。DistributedStateStore 的缓存 mutex 和 SQLite 操作不交叉，避免死锁。未来如需更高并发，可升级为分段锁（sharded lock）。

**替代方案**:
- `std::shared_mutex`（读写锁）: 读多写少场景更优，但 C++ 标准库的 shared_mutex 在某些平台上性能不如 mutex
- 无锁（lock-free）: 实现复杂度极高，且缓存场景不需要
- 分段锁: 过早优化，先用单 mutex 验证功能

### Decision 5: 配置方式 — 环境变量 + 构造参数

**选择**: 缓存配置通过 `CacheConfig` struct 传入构造函数，同时支持环境变量覆盖（`MINDBRIDGE_CACHE_*`）。

**理由**: 和现有 `MINDBRIDGE_STATE_BACKEND`、`MINDBRIDGE_STORAGE_BACKEND` 等环境变量风格一致。构造参数便于单元测试注入不同配置。

**配置项**:
- `MINDBRIDGE_CACHE_ENABLED` (bool, default true)
- `MINDBRIDGE_CACHE_MAX_SIZE` (size_t, default 10000)
- `MINDBRIDGE_CACHE_WRITE_TTL_SECONDS` (int, default 60)
- `MINDBRIDGE_CACHE_REFRESH_SECONDS` (int, default 30)
- `MINDBRIDGE_CACHE_ACCESS_TTL_SECONDS` (int, default 0, disabled)

### Decision 6: DistributedStateStore 缓存失效 — 写穿透 + 观察者通知

**选择**: `upsert_record` 时同步更新缓存（write-through），同时通过已有的 `change_observers_` 机制通知其他关注方。

**理由**: DistributedStateStore 已有 observer 模式（`add_change_observer`），`upsert_record` 后会 `notify_observers`。缓存作为 observer 注册，在收到变更通知时主动 `invalidate(key)`。这样即使缓存 miss 时从 DB 加载了旧数据，下一次写入时会自动失效。

**替代方案**:
- Write-behind（异步写回）: 会引入数据不一致窗口，对心理咨询场景不可接受
- TTL-only（纯过期）: 过期窗口内可能读到旧数据，但实现简单
- Read-through + TTL: 需要在 get 时阻塞加载，不如 write-through + observer 灵活

### Decision 7: PromptPrefix cache 集成点 — AgentLoop 层而非 PromptPrefix 本身

**选择**: 在 `AgentLoop::run()` / `run_stream()` 中，构建 prompt 前先查缓存。PromptPrefix 保持无状态。

**理由**: PromptPrefix 是无状态工具函数，不应持有缓存状态。AgentLoop 是调用方，负责缓存命中/未命中逻辑。这和 Java 中 `LoadingCache` 放在 Service 层而非 Model 层的模式一致。

## Risks / Trade-offs

**[内存压力]** 进程内缓存会增加内存占用。→ Mitigation: 通过 `MINDBRIDGE_CACHE_MAX_SIZE` 控制上限，默认 10000 条。每条 StateRecord 约 1-10KB，最坏情况 ~100MB。可通过 `MINDBRIDGE_CACHE_ENABLED=false` 完全禁用。

**[缓存一致性]** 写穿透 + observer 通知存在极短的不一致窗口（写 DB 和更新缓存之间）。→ Mitigation: 这个窗口在毫秒级，对心理咨询场景可接受。关键写操作（如 risk_level 变更）可通过 `invalidate(key)` 强制刷新。

**[后台刷新线程生命周期]** 刷新线程需要在缓存析构时干净退出。→ Mitigation: 析构函数中设置 shutdown flag + notify，join 线程。参考 Boost.Asio 的 `io_context::stop()` 模式。

**[SQLite 并发]** 缓存解决了大部分读并发，但写操作仍需序列化 SQLite 访问。→ Mitigation: 缓存层减少 SQLite 并发压力。SQLite WAL 模式下读写可并发，写操作内部已有序列化。未来可考虑连接池。

**[EmbeddingCache 迁移]** 现有 EmbeddingCache 使用自己的 LRU 实现，新 CaffeineCache 使用 LRU-K。→ Mitigation: 不强制迁移 EmbeddingCache，两者共存。未来可在 EmbeddingCache 中适配 CaffeineCache 接口。

## Open Questions

1. ~~是否需要实现 maximumWeight 按内存淘汰？~~ → 决定不在 v1 实现，后续按需添加。
2. ~~是否迁移 EmbeddingCache 到 CaffeineCache？~~ → 决定不迁移，两者共存。
3. 缓存 key 的 namespace 隔离：不同 user_id 的数据是否需要分片？→ 初期不分片，通过 key 前缀区分即可。
4. refreshAfterWrite 的刷新失败处理：刷新失败时是否继续返回旧值？→ 是，返回旧值 + 标记 stale，下次 get 时重试。
