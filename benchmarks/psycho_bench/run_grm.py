#!/usr/bin/env python3
"""
run_grm.py: fit Samejima-style Graded Response Model on PsychoBench judge scores.

Input: data/judge_scores.json with seven 1-10 ordinal judge ratings per response.
Output: data/grm/agent_abilities.csv + response_thetas.csv + model_summary.txt.

This path intentionally keeps the ordinal 1-10 scores instead of converting them
to pass/fail or binomial count-data.
"""

import csv
import json
import os
import sys
import argparse
from collections import defaultdict
from pathlib import Path

import numpy as np

DIMS = [
    "Active_Listening",
    "Empathy",
    "Safety",
    "Open_mindedness",
    "Clarity",
    "Boundaries",
    "Holistic",
]

MIN_SCORE = 1
MAX_SCORE = 10
N_CATEGORIES = MAX_SCORE - MIN_SCORE + 1


def load_judge_scores(scores_path):
    with open(scores_path, encoding="utf-8") as f:
        return json.load(f)


def build_grm_response_matrix(records):
    """Build a response-level ordinal matrix for GRM.

    Rows are generated responses (`agent::task_id`). Columns are the seven
    MentalChat16K-style judge dimensions. Values remain the original 1-10
    ordinal ratings; fitting subtracts 1 only because the Python `mirt` package
    expects categories coded 0..n_categories-1.
    """
    ordered_records = sorted(records, key=lambda r: (r["agent"], r["task_id"]))
    person_ids = []
    rows = []
    for record in ordered_records:
        person_ids.append(f"{record['agent']}::{record['task_id']}")
        row = []
        for dim in DIMS:
            score = int(record[dim])
            if score < MIN_SCORE or score > MAX_SCORE:
                raise ValueError(f"{record['agent']}::{record['task_id']} {dim} score out of range: {score}")
            row.append(score)
        rows.append(row)
    return np.array(rows, dtype=int), person_ids, list(DIMS)


def write_ordinal_matrix_csv(matrix, person_ids, item_ids, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["person_id", *item_ids])
        for person_id, row in zip(person_ids, matrix):
            writer.writerow([person_id, *[int(x) for x in row]])


def aggregate_agent_abilities(records, person_ids, theta, standard_error):
    by_agent = defaultdict(list)
    se_by_agent = defaultdict(list)
    score_by_agent = defaultdict(list)
    record_by_person = {
        f"{record['agent']}::{record['task_id']}": record
        for record in records
    }
    for person_id, theta_value, se_value in zip(person_ids, theta, standard_error):
        agent = person_id.split("::", 1)[0]
        by_agent[agent].append(float(theta_value))
        se_by_agent[agent].append(float(se_value))
        record = record_by_person[person_id]
        score_by_agent[agent].append(float(record.get("avg_score", sum(record[d] for d in DIMS) / len(DIMS))))

    rows = []
    for agent in sorted(by_agent):
        values = np.array(by_agent[agent], dtype=float)
        ses = np.array(se_by_agent[agent], dtype=float)
        scores = np.array(score_by_agent[agent], dtype=float)
        rows.append({
            "agent": agent,
            "theta": float(values.mean()),
            "theta_sd": float(values.std(ddof=1)) if len(values) > 1 else 0.0,
            "theta_se_mean": float(values.std(ddof=1) / np.sqrt(len(values))) if len(values) > 1 else float(ses.mean()),
            "mean_judge_score": float(scores.mean()),
            "n_responses": int(len(values)),
        })
    rows.sort(key=lambda r: r["theta"], reverse=True)

    best_raw = next((r for r in rows if r["agent"] != "mindbridge"), None)
    mindbridge = next((r for r in rows if r["agent"] == "mindbridge"), None)
    if best_raw and mindbridge:
        target = best_raw["theta"] * 1.02 if best_raw["theta"] >= 0 else best_raw["theta"] + abs(best_raw["theta"]) * 0.02
        mindbridge["best_raw_agent"] = best_raw["agent"]
        mindbridge["best_raw_theta"] = best_raw["theta"]
        mindbridge["target_theta_2pct"] = target
        mindbridge["theta_ratio_vs_best_raw"] = (
            mindbridge["theta"] / best_raw["theta"] if best_raw["theta"] != 0 else float("nan")
        )
        mindbridge["meets_2pct_target"] = mindbridge["theta"] >= target
    return rows


def write_agent_abilities(rows, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "agent",
        "theta",
        "theta_sd",
        "theta_se_mean",
        "mean_judge_score",
        "n_responses",
        "best_raw_agent",
        "best_raw_theta",
        "target_theta_2pct",
        "theta_ratio_vs_best_raw",
        "meets_2pct_target",
    ]
    with open(path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def write_response_thetas(records, person_ids, theta, standard_error, path):
    by_person = {
        f"{record['agent']}::{record['task_id']}": record
        for record in records
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["person_id", "agent", "task_id", "source", "topic", "difficulty", "theta", "theta_se", "avg_score"],
        )
        writer.writeheader()
        for person_id, theta_value, se_value in zip(person_ids, theta, standard_error):
            record = by_person[person_id]
            writer.writerow({
                "person_id": person_id,
                "agent": record["agent"],
                "task_id": record["task_id"],
                "source": record.get("source", ""),
                "topic": record.get("topic", ""),
                "difficulty": record.get("difficulty", ""),
                "theta": float(theta_value),
                "theta_se": float(se_value),
                "avg_score": record.get("avg_score", ""),
            })


def assert_mindbridge_target(rows, baseline_agent="qwen-raw", min_improvement=0.02):
    by_agent = {row["agent"]: row for row in rows}
    if baseline_agent not in by_agent:
        raise AssertionError(f"missing baseline agent: {baseline_agent}")
    if "mindbridge" not in by_agent:
        raise AssertionError("missing mindbridge agent")

    baseline_theta = float(by_agent[baseline_agent]["theta"])
    mindbridge_theta = float(by_agent["mindbridge"]["theta"])
    target = (
        baseline_theta * (1.0 + min_improvement)
        if baseline_theta >= 0
        else baseline_theta + abs(baseline_theta) * min_improvement
    )
    if mindbridge_theta < target:
        raise AssertionError(
            "MindBridge GRM target not met: "
            f"mindbridge theta={mindbridge_theta:.6f}, "
            f"{baseline_agent} theta={baseline_theta:.6f}, "
            f"required theta>={target:.6f}"
        )


def run_grm(scores_path, output_dir, max_iter=500, tol=1e-4, score_method="MAP"):
    records = load_judge_scores(scores_path)
    matrix_1_to_10, person_ids, item_ids = build_grm_response_matrix(records)
    matrix_0_to_9 = matrix_1_to_10 - MIN_SCORE

    try:
        import mirt
    except ImportError as exc:
        raise RuntimeError("Python package `mirt` is required. Install with: pip install 'mirt[pandas]'") from exc

    output_dir = Path(output_dir)
    write_ordinal_matrix_csv(matrix_1_to_10, person_ids, item_ids, output_dir / "ordinal_scores.csv")

    result = mirt.fit_mirt(
        matrix_0_to_9,
        model="GRM",
        n_categories=N_CATEGORIES,
        item_names=item_ids,
        max_iter=max_iter,
        tol=tol,
        verbose=False,
    )
    scores = mirt.fscores(result, matrix_0_to_9, method=score_method, person_ids=person_ids)
    theta = np.array(scores.theta, dtype=float).reshape(-1)
    standard_error = np.array(scores.standard_error, dtype=float).reshape(-1)

    agent_rows = aggregate_agent_abilities(records, person_ids, theta, standard_error)
    write_agent_abilities(agent_rows, output_dir / "agent_abilities.csv")
    write_response_thetas(records, person_ids, theta, standard_error, output_dir / "response_thetas.csv")
    (output_dir / "model_summary.txt").write_text(str(result.summary()), encoding="utf-8")

    return agent_rows


def main():
    parser = argparse.ArgumentParser(description="Fit GRM on PsychoBench ordinal judge scores.")
    parser.add_argument("--require-target", action="store_true",
                        help="exit non-zero unless MindBridge theta clears the requested qwen-raw margin")
    parser.add_argument("--min-improvement", type=float, default=0.02,
                        help="required MindBridge improvement over qwen-raw as a fraction; default 0.02")
    args = parser.parse_args()

    base_dir = Path(__file__).resolve().parent
    scores_path = base_dir / "data" / "judge_scores.json"
    output_dir = base_dir / "data" / "grm"
    if not scores_path.exists():
        print(f"ERROR: {scores_path} 不存在")
        print("请先运行 psycho_bench.py 生成 judge_scores.json")
        sys.exit(1)

    max_iter = int(os.environ.get("PSYCHOBENCH_GRM_MAX_ITER", "500"))
    score_method = os.environ.get("PSYCHOBENCH_GRM_SCORE_METHOD", "MAP")
    rows = run_grm(scores_path, output_dir, max_iter=max_iter, score_method=score_method)

    print("=" * 60)
    print("GRM 训练完成")
    print("=" * 60)
    print(f"  模型能力: {output_dir / 'agent_abilities.csv'}")
    print(f"  回复级 theta: {output_dir / 'response_thetas.csv'}")
    print(f"  有序评分矩阵: {output_dir / 'ordinal_scores.csv'}")
    print("\n── GRM 模型能力排名 ──")
    print(f"  {'Agent':<20} {'theta':>10} {'mean_score':>10} {'n':>5}")
    print("  " + "-" * 49)
    for row in rows:
        print(f"  {row['agent']:<20} {row['theta']:>10.3f} {row['mean_judge_score']:>10.3f} {row['n_responses']:>5}")
    mindbridge = next((r for r in rows if r["agent"] == "mindbridge"), None)
    if mindbridge and mindbridge.get("best_raw_agent"):
        print()
        print(f"Best raw: {mindbridge['best_raw_agent']} theta={mindbridge['best_raw_theta']:.3f}")
        print(f"MindBridge theta={mindbridge['theta']:.3f}")
        print(f"2% target theta={mindbridge['target_theta_2pct']:.3f}")
        print(f"Meets 2% target: {mindbridge['meets_2pct_target']}")
        if args.min_improvement != 0.02:
            target = (
                mindbridge["best_raw_theta"] * (1.0 + args.min_improvement)
                if mindbridge["best_raw_theta"] >= 0
                else mindbridge["best_raw_theta"] + abs(mindbridge["best_raw_theta"]) * args.min_improvement
            )
            print(f"{args.min_improvement:.1%} target theta={target:.3f}")
            print(f"Meets {args.min_improvement:.1%} target: {mindbridge['theta'] >= target}")

    if args.require_target:
        try:
            assert_mindbridge_target(rows, min_improvement=args.min_improvement)
        except AssertionError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            sys.exit(2)


if __name__ == "__main__":
    main()
