# MindBridge 上下文管理策略

## 目标

避免把所有规则和历史一次性塞进上下文，减少模型分心与幻觉，提升执行稳定性。

## 分层策略

### Layer 0: 入口索引（常驻）
- 文件：`AGENTS.md`
- 内容：项目定位、关键规则、必读文档索引、验证命令
- 约束：约 100 行内，禁止堆叠细节

### Layer 1: 任务相关文档（按需加载）
- `docs/architecture.md`
- `docs/harness_engineering.md`
- `docs/model_strategy.md`
- `docs/interview_notes.md`
- `mindbridge_harness/configs/feature_status.json`

只读与当前任务强相关的文档，不相关内容不加载。

### Layer 2: 代码上下文（增量加载）
- 先读接口头文件，再读实现文件。
- 优先读目标模块目录，避免全仓扫描。
- 每次改动后先本地验证，再扩展读取范围。

## Smart Zone / Dumb Zone

### Smart Zone（应长期保持）
- 当前任务所需文档与代码
- 最近修改文件
- 验证失败日志

### Dumb Zone（应尽量排除）
- 无关子项目（例如 `deer-flow/`、`agentscope/`、`OpenHarness/`）的大量文档
- 历史聊天内容中已过时的决策
- 与当前任务不相关的大体量文件

## 执行规则

1. 开始任务先看 `AGENTS.md`。
2. 根据任务类型选 1 到 3 个文档按需加载。
3. 改动后运行 `scripts/verify_mindbridge.sh`。
4. 发现新规则或新坑，回写文档与 `feature_status.json`。

## 反模式

- 把 AGENTS.md 写成超级 System Prompt。
- 每次都全量加载 docs 与源码。
- 出错后只修代码，不更新规则和验证脚本。
