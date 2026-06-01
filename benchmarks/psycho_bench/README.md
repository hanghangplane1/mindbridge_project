# PsychoBench

基于 IRT 的心理咨询 AI 评测框架。

## 快速开始

```bash
# 1. 设置环境变量
export DEEPSEEK_API_KEY="..."
export QWEN_API_KEY="..."
export DOUBAO_API_KEY="..."
export MIMO_API_KEY="..."

# 2. 安装依赖
pip install openai pyyaml "mirt[pandas]"

# 3. 运行 Benchmark（85场景×4模型×2=680次API调用，约30-40分钟）
python psycho_bench.py

# 4. 运行 GRM（推荐：直接使用 7 维 1-10 ordinal scores）
python run_grm.py

# 5. 重跑 MindBridge rows 并要求 GRM 超过 qwen-raw 2%（需要 Gateway + MIMO_API_KEY）
python rerun_mindbridge_grm.py

# 6. 运行旧 count-data IRT（仅作为历史/工程近似对照）
python run_irt.py
```

## 数据分布

- CPsyCounE: 45 个多轮对话（9 主题 × 5 案例）
- PsyQA: 30 条单轮问答
- 手写危机场景: 10 条（补足 L3）
- **总计: 85 个场景** (L1=26, L2=50, L3=9)

## 输出

```
data/
├── tasks_75.json        # 75 个测试场景
├── progress.jsonl       # 详细进度（断点续跑用）
├── responses.jsonl      # IRT 输入（7 维连续评分转 count-data）
├── judge_scores.json    # 7 维度详细分数
├── grm/
│   ├── ordinal_scores.csv   # GRM 输入：回复级 7 维 1-10 有序评分
│   ├── response_thetas.csv  # GRM 回复级 theta
│   └── agent_abilities.csv  # GRM agent 级 theta（按回复聚合）
└── irt/
    └── 1d_1pl/
        ├── abilities.csv  # 模型能力 θ
        └── items.csv      # 场景难度 b
```

## 被测模型

| Agent | 模型 | 说明 |
|-------|------|------|
| deepseek-raw | DeepSeek-V4-Flash | 裸 LLM |
| qwen-raw | Qwen3.6-Flash | 裸 LLM |
| doubao-raw | Doubao-Seed-2.0-Lite | 裸 LLM |
| mindbridge | MindBridge Gateway | 系统（Qwen + RAG + MCP） |

MindBridge 分支走 Gateway 根路径 JSON-RPC，但请求体必须使用前端文本协议：
`method=message/send` 且文本放在 `params.message.parts[{kind:"text"}]`。
不要使用 OpenAI 兼容的 `chat/completions` / `params.messages` 形状，否则后端拿不到文本，可能落入语音转写失败兜底。
由于 Gateway 文本入口是单条 A2A message，而不是 OpenAI 多轮 `messages[]`，
benchmark 会把历史对话包装成明确的“历史对话 / 当前求助者最新发言 / 你的回复”文本，
并要求 MindBridge 只接续回复最新求助者，避免把整段对话当成材料点评。

## 评测与 IRT

PsychoBench 使用 MentalChat16K 风格的 7 个维度做 1-10 有序评分。推荐入口是
`run_grm.py`：它用 Python `mirt` 包拟合 Graded Response Model（GRM），直接保留
7 个维度的 ordinal score。实现上每条生成回复（`agent::task_id`）作为 respondent，
7 个评分维度作为 ordered items，最终把回复级 theta 按 agent 聚合成模型能力。
验收 MindBridge 是否超过 qwen-raw 2% 时运行：

```bash
python run_grm.py --require-target
```

该命令会在 MindBridge 的 GRM theta 未达到 `qwen-raw * 1.02` 时非零退出。
如果修改了 MindBridge PromptPolicy 或 runtime，需要让当前 Gateway 指向新二进制后重跑
MindBridge rows 并重新 judge：

```bash
# 检查任务选择、文件路径和 key 状态，不调用模型
python rerun_mindbridge_grm.py --dry-run

# 只重跑少量任务做 smoke
python rerun_mindbridge_grm.py --task-id cp_Mental\ Disease_4 --no-require-target

# 重跑全部 MindBridge rows，合并 judge_scores.json，并执行 GRM 2% gate
python rerun_mindbridge_grm.py
```

`rerun_mindbridge_grm.py` 只替换 `agent=mindbridge` 的指定任务 rows，保留
qwen/deepseek/doubao raw baseline 不变；写入前会备份旧 `judge_scores.json`。

`run_irt.py` 仍保留为历史对照：它会从 `judge_scores.json` 重新生成
`responses.jsonl`，不做 `avg_score >= 6` 这类 pass/fail 二值化，而是把 7 个维度
分别从 1-10 映射为 0-9 个质量点并写成 `{"successes": score_points, "trials": 63}`。
这只是为了复用二项/count-data IRT 的工程近似，不作为最终心理测量学口径。

## 评测维度（来自 MentalChat16K）

1. Active Listening（主动倾听）
2. Empathy & Validation（共情确认）
3. Safety & Trustworthiness（安全可信）
4. Open-mindedness（非评判）
5. Clarity & Encouragement（清晰鼓励）
6. Boundaries & Ethical（伦理边界）
7. Holistic Approach（整体方法）
