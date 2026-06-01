# PsychoBench 实现任务

## 任务列表

- [ ] 1. 创建项目目录和配置文件
  - 创建 `benchmarks/psycho_bench/` 目录
  - 编写 `config.yaml`（API 地址和模型名，密钥从环境变量读取）
  - 安装依赖：`pip install openai pyyaml`

- [ ] 2. 编写数据加载模块
  - `load_cpsycoune()`：从 CPsyCounE 加载 45 个多轮对话
    - 方案B：取除最后一轮支持者发言外的所有轮次作为 context
    - 最后一轮求助者发言作为 prompt
  - `load_psyqa()`：从 PsyQA_example.json 随机抽 30 条
  - `auto_difficulty()`：关键词自动标注 L1/L2/L3
  - 合并输出 `tasks_75.json`

- [ ] 3. 编写 API 调用模块
  - `call_agent(cfg, prompt, context)`：调用被测模型生成回复
    - 将 context 转为 OpenAI messages 格式
    - 限速：每次调用间隔 0.5 秒
  - `call_mindbridge(prompt, context)`：调用 MindBridge Gateway
    - HTTP POST 到 `localhost:8090/v1`
    - MindBridge 内部走 Orchestrator → Counselor（含 RAG + MCP）
  - `call_judge(cfg, prompt, response)`：调用 MiMo Judge 打 7 维分
    - temperature=0 确保确定性
    - 解析 JSON 返回

- [ ] 4. 编写主脚本 `psycho_bench.py`
  - 加载场景 → 调用 4 个模型（deepseek/qwen/doubao/mindbridge） → Judge 打分 → 二值化 → 保存
  - 输出 `data/responses.jsonl`（IRT 输入，4 agents × 75 tasks）
  - 输出 `data/judge_scores.json`（详细分数）
  - 加断点续跑功能（已跑完的跳过）

- [ ] 5. 编写 IRT 训练脚本 `run_irt.py`
  - 调用 agent-psychometrics 的 `train.py`
  - 输出 `data/irt/1d_1pl/abilities.csv` 和 `items.csv`

- [ ] 6. 编写结果分析脚本 `analyze.py`
  - 读取 abilities.csv 和 items.csv
  - 输出模型能力排名表（θ 值）
  - 输出场景难度排名表（b 值）
  - 输出 L1/L2/L3 各难度通过率
  - 输出 9 主题各主题通过率
  - 输出消融分析：qwen-raw vs mindbridge 的 Δθ

- [ ] 7. 编写 README.md
  - 项目说明
  - 运行步骤
  - 预期输出
  - 论文贡献点
