# PsychoBench: 基于 IRT 的心理咨询 AI 评测框架

## 背景

MindBridge 已有一套基于合约测试的 benchmark（`mindbridge_tasks.json`，5 个任务），覆盖了硬约束验证（正则匹配、意图分类、风险等级）。但缺少**语义层面的质量评测**——即"回复质量好不好"，而不仅仅是"格式对不对"。

本方案设计一套完整的心理咨询 AI 评测框架，补上这个缺口。

## 核心思路

```
┌─────────────────────────────────────────────────────────────┐
│                    PsychoBench 核心思路                      │
└─────────────────────────────────────────────────────────────┘

  数据源                    评测方法                  产出
  ──────                    ────────                  ────
  CPsyCounE (45对话)   ──┐
                         ├──▶ LLM-as-Judge (7维度) ──▶ 0/1矩阵 ──▶ IRT
  PsyQA (30条问答)    ──┘        ▲                                 │
                                 │                                 ▼
                          MiMo-v2.5-Pro                    θ_模型能力 + b_场景难度
```

## 与现有 Benchmark 的关系

| | 现有合约测试 | PsychoBench（本方案） |
|---|---|---|
| **评测内容** | 格式/意图/风险等级 | 语义质量（共情、安全、专业性） |
| **验证方式** | 正则匹配 | LLM-as-Judge 7维度打分 |
| **输出** | pass/fail | 0/1 + 详细分数 |
| **数学基础** | 无 | IRT（项目反应理论） |
| **互补关系** | 硬约束 | 软偏好 |

两者互补，不冲突。合约测试确保"不出错"，PsychoBench 评测"做得好"。

## 为什么用 IRT

现有心理咨询 Benchmark（PsyLLM、MentalChat16K、XInsight）都只算平均分。IRT 的优势：

1. **分离模型能力和场景难度**——平均分混在一起，IRT 分开
2. **能预测新场景表现**——已知 θ 和 b，直接算 P(pass)
3. **找到最有区分度的场景**——b 值适中、区分度高的场景最有价值
4. **60 年历史，方法成熟**——Rasch 1960，SAT/GRE 都用

**参考文献：** Agent Psychometrics (ICLR 2026 Workshop, arXiv:2604.00594) 首次将 IRT 应用于 AI 评测，但只做了 coding benchmark。PsychoBench 是首个将 IRT 应用于心理咨询领域的评测框架。

## 调研依据

基于 10 篇论文的系统调研，详见：
`/home/hang/work/a2a/benchmark-research-skill/output/Benchmark_Surveys/surveys/2026-05-30-mental-health/benchmark-survey.md`

关键发现：
- **MentalChat16K** (2503.13509) 定义了 7 个评测维度，本方案直接采用
- **PsyLLM** (2505.15715) 使用 CPsyCounE 数据集，本方案复用
- **所有成熟 Benchmark 都用 LLM-as-Judge**，不用人工打分
