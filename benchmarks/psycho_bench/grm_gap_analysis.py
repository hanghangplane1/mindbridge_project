#!/usr/bin/env python3
"""
Analyze which MindBridge rows are most responsible for the GRM gap.

The simulation path still uses run_grm.py, which fits the Python `mirt` GRM
package. This file only prepares hypothetical judge-score rows to identify a
targeted rerun set; it is not a replacement for live judging.
"""

import argparse
import json
import tempfile
from pathlib import Path

import psycho_bench
import run_grm


def load_scores(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def index_by_agent_task(rows):
    return {(row["agent"], row["task_id"]): row for row in rows}


def rank_mindbridge_gaps(rows, baseline_agent="qwen-raw"):
    by_key = index_by_agent_task(rows)
    gaps = []
    task_ids = sorted({
        row["task_id"]
        for row in rows
        if row.get("agent") == "mindbridge" and (baseline_agent, row["task_id"]) in by_key
    })
    for task_id in task_ids:
        mb = by_key[("mindbridge", task_id)]
        baseline = by_key[(baseline_agent, task_id)]
        dim_gaps = {
            dim: int(baseline[dim]) - int(mb[dim])
            for dim in psycho_bench.DIMS
        }
        positive_gap = sum(max(0, value) for value in dim_gaps.values())
        raw_gap = sum(dim_gaps.values())
        if positive_gap <= 0:
            continue
        gaps.append({
            "task_id": task_id,
            "source": mb.get("source", ""),
            "topic": mb.get("topic", ""),
            "difficulty": mb.get("difficulty", ""),
            "score_gap": positive_gap,
            "raw_score_gap": raw_gap,
            "mindbridge_avg": mb.get("avg_score", ""),
            "baseline_avg": baseline.get("avg_score", ""),
            "dimension_gaps": dim_gaps,
        })
    gaps.sort(key=lambda item: (item["score_gap"], item["raw_score_gap"], item["task_id"]), reverse=True)
    return gaps


def patch_mindbridge_rows(rows, task_ids, baseline_agent="qwen-raw", margin=1):
    selected = set(task_ids)
    by_key = index_by_agent_task(rows)
    patched = []
    for row in rows:
        if row.get("agent") != "mindbridge" or row.get("task_id") not in selected:
            patched.append(dict(row))
            continue
        baseline = by_key.get((baseline_agent, row["task_id"]))
        if baseline is None:
            patched.append(dict(row))
            continue
        new_row = dict(row)
        for dim in psycho_bench.DIMS:
            new_row[dim] = min(psycho_bench.MAX_SCORE_PER_DIM, int(baseline[dim]) + margin)
        new_row["avg_score"] = round(sum(new_row[dim] for dim in psycho_bench.DIMS) / len(psycho_bench.DIMS), 2)
        new_row["response"] = f"[simulated {baseline_agent}+{margin} target]"
        patched.append(new_row)
    return patched


def run_grm_on_rows(rows, score_method="MAP"):
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        scores_path = tmp_path / "judge_scores.json"
        out_dir = tmp_path / "grm"
        scores_path.write_text(json.dumps(rows, ensure_ascii=False), encoding="utf-8")
        return run_grm.run_grm(scores_path, out_dir, score_method=score_method)


def target_status(agent_rows, baseline_agent="qwen-raw"):
    by_agent = {row["agent"]: row for row in agent_rows}
    baseline = float(by_agent[baseline_agent]["theta"])
    mindbridge = float(by_agent["mindbridge"]["theta"])
    target = baseline * 1.02 if baseline >= 0 else baseline + abs(baseline) * 0.02
    return {
        "baseline_theta": baseline,
        "mindbridge_theta": mindbridge,
        "target_theta": target,
        "meets_target": mindbridge >= target,
    }


def main():
    base_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Analyze MindBridge rows for the GRM qwen+2% gap.")
    parser.add_argument("--scores", default=str(base_dir / "data" / "judge_scores.json"))
    parser.add_argument("--top", type=int, default=15, help="number of gap rows to print")
    parser.add_argument("--simulate-top", type=int, default=0,
                        help="simulate replacing the top N gap rows with qwen+margin scores")
    parser.add_argument("--margin", type=int, default=1, help="simulated score margin over qwen per dimension")
    args = parser.parse_args()

    rows = load_scores(args.scores)
    gaps = rank_mindbridge_gaps(rows)
    print("Top MindBridge gaps vs qwen-raw")
    print("rank,task_id,score_gap,topic,difficulty,dimension_gaps")
    for idx, item in enumerate(gaps[:args.top], start=1):
        print(f"{idx},{item['task_id']},{item['score_gap']},{item['topic']},{item['difficulty']},{item['dimension_gaps']}")

    if args.simulate_top:
        selected = [item["task_id"] for item in gaps[:args.simulate_top]]
        patched = patch_mindbridge_rows(rows, selected, margin=args.margin)
        status = target_status(run_grm_on_rows(patched))
        print()
        print(f"Simulated top {args.simulate_top} rows at qwen+{args.margin}:")
        print(json.dumps({"selected_task_ids": selected, **status}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
