#!/usr/bin/env python3
"""
Rerun MindBridge rows, re-judge them, merge judge_scores.json, then run GRM.

This script is intentionally scoped to the MindBridge candidate rows. Existing
raw-model rows stay fixed, so the qwen baseline remains comparable while prompt
or runtime changes in MindBridge can be re-evaluated.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import yaml

import psycho_bench


def load_tasks(tasks_path):
    with open(tasks_path, encoding="utf-8") as f:
        return json.load(f)


def load_scores(scores_path):
    with open(scores_path, encoding="utf-8") as f:
        return json.load(f)


def write_scores(scores_path, rows):
    with open(scores_path, "w", encoding="utf-8") as f:
        json.dump(rows, f, ensure_ascii=False, indent=2)


def build_judged_record(task, response, scores, agent="mindbridge"):
    missing = [dim for dim in psycho_bench.DIMS if dim not in scores]
    if missing:
        raise ValueError("judge scores missing dimensions: " + ", ".join(missing))
    clean_scores = {}
    for dim in psycho_bench.DIMS:
        value = int(scores[dim])
        if value < psycho_bench.MIN_SCORE_PER_DIM or value > psycho_bench.MAX_SCORE_PER_DIM:
            raise ValueError(f"{dim} score out of range: {value}")
        clean_scores[dim] = value

    avg = sum(clean_scores.values()) / len(psycho_bench.DIMS)
    return {
        "agent": agent,
        "task_id": task["id"],
        "source": task["source"],
        "topic": task["topic"],
        "difficulty": task["difficulty"],
        "response": response[:300],
        "avg_score": round(avg, 2),
        **clean_scores,
    }


def replace_agent_records(existing_rows, replacement_rows, agent):
    replacements = {(row["agent"], row["task_id"]): row for row in replacement_rows}
    replaced_keys = set()
    merged = []
    for row in existing_rows:
        key = (row["agent"], row["task_id"])
        if row.get("agent") == agent and key in replacements:
            merged.append(replacements[key])
            replaced_keys.add(key)
        else:
            merged.append(row)

    for key in sorted(set(replacements) - replaced_keys):
        merged.append(replacements[key])
    return merged


def completed_task_ids(rows, agent):
    return {
        row["task_id"]
        for row in rows
        if row.get("agent") == agent and row.get("task_id")
    }


def select_tasks(tasks, task_ids=None, limit=None):
    selected = tasks
    if task_ids:
        wanted = set(task_ids)
        selected = [task for task in tasks if task["id"] in wanted]
        missing = sorted(wanted - {task["id"] for task in selected})
        if missing:
            raise ValueError("unknown task ids: " + ", ".join(missing))
    if limit is not None:
        selected = selected[:limit]
    return selected


def require_env(name):
    if not os.environ.get(name):
        raise RuntimeError(f"{name} is required")


def judge_with_retries(judge_client, judge_model, task, context_text, response, attempts=3, sleep_seconds=0.5):
    last_error = None
    for attempt in range(1, attempts + 1):
        try:
            return psycho_bench.call_judge(judge_client, judge_model, task["prompt"], context_text, response)
        except Exception as exc:
            last_error = exc
            print(f"judge retry {attempt}/{attempts} for {task['id']}: {exc}", flush=True)
            time.sleep(sleep_seconds)
    raise last_error


def rerun_mindbridge_rows(tasks, judge_client, judge_model, sleep_seconds=0.5, judge_retries=3):
    for index, task in enumerate(tasks, start=1):
        response = psycho_bench.call_agent_jsonrpc(
            task["prompt"],
            task.get("context"),
            context_id=task["id"],
        )
        time.sleep(sleep_seconds)
        context_text = "\n".join(task.get("context", []))
        scores = judge_with_retries(
            judge_client,
            judge_model,
            task,
            context_text,
            response,
            attempts=judge_retries,
            sleep_seconds=sleep_seconds,
        )
        row = build_judged_record(task, response, scores)
        print(f"{index}/{len(tasks)} {task['id']}: avg={row['avg_score']:.2f}", flush=True)
        time.sleep(sleep_seconds)
        yield row


def main():
    base_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Rerun MindBridge judge rows and verify GRM target.")
    parser.add_argument("--task-id", action="append", dest="task_ids",
                        help="task id to rerun; repeat for multiple tasks")
    parser.add_argument("--limit", type=int, default=None,
                        help="rerun only the first N selected tasks")
    parser.add_argument("--no-require-target", action="store_true",
                        help="fit GRM without enforcing the qwen+2%% target")
    parser.add_argument("--min-improvement", type=float, default=0.02,
                        help="required MindBridge improvement over qwen-raw when target gate is on")
    parser.add_argument("--sleep", type=float, default=0.5,
                        help="seconds to sleep between model/judge calls")
    parser.add_argument("--judge-retries", type=int, default=3,
                        help="retry judge calls when JSON parsing or HTTP fails")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate files/task selection/env names without calling model or judge")
    parser.add_argument("--resume", action="store_true",
                        help="skip selected tasks already present as mindbridge rows in judge_scores.json")
    args = parser.parse_args()

    config_path = base_dir / "config.yaml"
    config = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    output_dir = base_dir / config["data"]["output_dir"]
    tasks_path = output_dir / "tasks_75.json"
    scores_path = output_dir / "judge_scores.json"
    if not tasks_path.exists() or not scores_path.exists():
        raise SystemExit("tasks_75.json and judge_scores.json must exist; run psycho_bench.py first")

    judge_cfg = config["judge"]

    tasks = select_tasks(load_tasks(tasks_path), task_ids=args.task_ids, limit=args.limit)
    if not tasks:
        raise SystemExit("no tasks selected")
    if args.dry_run:
        judge_key_state = "SET" if os.environ.get(judge_cfg["env_key"]) else "UNSET"
        print(f"tasks selected: {len(tasks)}")
        print(f"judge env {judge_cfg['env_key']}: {judge_key_state}")
        print(f"scores file: {scores_path}")
        print(f"GRM target gate: {'off' if args.no_require_target else 'on'}")
        print(f"min improvement: {args.min_improvement:.1%}")
        return
    require_env(judge_cfg["env_key"])

    old_rows = load_scores(scores_path)
    backup_path = scores_path.with_suffix(".json.bak")
    shutil.copy2(scores_path, backup_path)
    print(f"backup={backup_path}")

    if args.resume:
        done = completed_task_ids(old_rows, "mindbridge")
        tasks = [task for task in tasks if task["id"] not in done]
        print(f"resume: remaining tasks={len(tasks)}")

    judge_client = psycho_bench.make_client(judge_cfg)
    judge_model = judge_cfg["model"]
    merged = old_rows
    for row in rerun_mindbridge_rows(
        tasks,
        judge_client,
        judge_model,
        sleep_seconds=args.sleep,
        judge_retries=args.judge_retries,
    ):
        merged = replace_agent_records(merged, [row], "mindbridge")
        write_scores(scores_path, merged)
        print(f"checkpointed {row['task_id']} -> {scores_path}", flush=True)

    cmd = [sys.executable, str(base_dir / "run_grm.py")]
    if not args.no_require_target:
        cmd.append("--require-target")
        cmd.extend(["--min-improvement", str(args.min_improvement)])
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
