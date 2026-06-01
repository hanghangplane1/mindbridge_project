#!/usr/bin/env python3
"""
run_irt.py: 调用 agent-psychometrics 的 IRT 训练

输入: data/responses.jsonl
输出: data/irt/1d_1pl/abilities.csv + items.csv
"""

import subprocess
import sys
import os
import json
import yaml

DIMS = [
    "Active_Listening",
    "Empathy",
    "Safety",
    "Open_mindedness",
    "Clarity",
    "Boundaries",
    "Holistic",
]


def score_record_to_irt_response(record):
    successes = 0
    for dim in DIMS:
        score = int(record[dim])
        if score < 1 or score > 10:
            raise ValueError(f"{dim} score out of range: {score}")
        successes += score - 1
    return {"successes": successes, "trials": len(DIMS) * 9}


def build_continuous_irt_responses(scores):
    responses_by_agent = {}
    for record in scores:
        agent = record["agent"]
        task_id = record["task_id"]
        responses_by_agent.setdefault(agent, {})[task_id] = score_record_to_irt_response(record)
    return [
        {"subject_id": agent, "responses": responses_by_agent[agent]}
        for agent in sorted(responses_by_agent)
    ]


def regenerate_continuous_responses(scores_path, responses_path):
    with open(scores_path, encoding="utf-8") as f:
        scores = json.load(f)
    rows = build_continuous_irt_responses(scores)
    with open(responses_path, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    return rows


def main():
    # 加载配置
    config_path = os.path.join(os.path.dirname(__file__), "config.yaml")
    with open(config_path) as f:
        config = yaml.safe_load(f)

    base_dir = os.path.dirname(os.path.abspath(__file__))
    data_path = os.path.join(base_dir, "data", "responses.jsonl")
    scores_path = os.path.join(base_dir, "data", "judge_scores.json")
    output_dir = os.path.join(base_dir, "data", "irt")
    train_script = config["irt"]["train_script"]

    if not os.path.exists(scores_path):
        print(f"ERROR: {scores_path} 不存在")
        print("请先运行 psycho_bench.py 生成 judge_scores.json")
        sys.exit(1)

    rows = regenerate_continuous_responses(scores_path, data_path)
    print(f"Generated continuous IRT input: {data_path} ({len(rows)} agents)")

    cmd = [
        sys.executable,
        train_script,
        "--data_path", data_path,
        "--output_dir", output_dir,
        "--model", config["irt"]["model"],
        "--epochs", str(config["irt"]["epochs"]),
        "--dims", "1",
    ]
    if config["irt"].get("seed") is not None:
        cmd.extend(["--seed", str(config["irt"]["seed"])])

    print(f"Running: {' '.join(cmd)}")
    print()
    subprocess.run(cmd, check=True)

    print()
    print("=" * 60)
    print("IRT 训练完成")
    print("=" * 60)
    print(f"  模型能力: {output_dir}/1d_1pl/abilities.csv")
    print(f"  场景难度: {output_dir}/1d_1pl/items.csv")

    # 打印结果
    print("\n── 模型能力排名 ──")
    abilities_file = os.path.join(output_dir, "1d_1pl", "abilities.csv")
    if os.path.exists(abilities_file):
        with open(abilities_file) as f:
            lines = f.readlines()
        print(f"  {'Agent':<25} {'θ':>8} {'std':>8}")
        print("  " + "-" * 44)
        for line in lines[1:]:  # skip header
            parts = line.strip().split(",")
            if len(parts) >= 3:
                print(f"  {parts[0]:<25} {float(parts[1]):>8.3f} {float(parts[2]):>8.3f}")

    print("\n── 场景难度 Top 10（最难）──")
    items_file = os.path.join(output_dir, "1d_1pl", "items.csv")
    if os.path.exists(items_file):
        with open(items_file) as f:
            lines = f.readlines()
        items = []
        for line in lines[1:]:
            parts = line.strip().split(",")
            if len(parts) >= 3:
                items.append((parts[0], float(parts[1]), float(parts[2])))
        items.sort(key=lambda x: -x[1])
        print(f"  {'Task':<35} {'b':>8} {'std':>8}")
        print("  " + "-" * 54)
        for task_id, b, std in items[:10]:
            print(f"  {task_id:<35} {b:>8.3f} {std:>8.3f}")

        print("\n── 场景难度 Bottom 5（最简单）──")
        print(f"  {'Task':<35} {'b':>8} {'std':>8}")
        print("  " + "-" * 54)
        for task_id, b, std in items[-5:]:
            print(f"  {task_id:<35} {b:>8.3f} {std:>8.3f}")


if __name__ == "__main__":
    main()
