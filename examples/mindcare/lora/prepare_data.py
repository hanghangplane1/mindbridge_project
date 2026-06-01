#!/usr/bin/env python3
import argparse
import json
import random
from pathlib import Path


def load_items(path: Path):
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, list):
        raise ValueError("Input dataset must be a list")
    return data


def normalize_record(item):
    instruction = (item.get("instruction") or "").strip()
    user_input = (item.get("input") or "").strip()
    output = (item.get("output") or "").strip()
    system = (item.get("system") or "").strip()
    if not output:
        return None

    if system:
        system_prompt = system
    else:
        system_prompt = instruction or "You are a licensed psychological counselor."

    user_prompt = user_input if user_input else instruction
    if not user_prompt:
        return None

    return {
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
            {"role": "assistant", "content": output},
        ]
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="PsyQA_alpaca_labelled.json")
    parser.add_argument("--output-dir", required=True, help="Output folder")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--train-ratio", type=float, default=0.9)
    args = parser.parse_args()

    random.seed(args.seed)
    items = load_items(Path(args.input))
    records = [normalize_record(x) for x in items]
    records = [x for x in records if x is not None]
    random.shuffle(records)

    split_idx = int(len(records) * args.train_ratio)
    train = records[:split_idx]
    eval_set = records[split_idx:]

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    train_path = out_dir / "train_data.jsonl"
    eval_path = out_dir / "eval_data.jsonl"

    with train_path.open("w", encoding="utf-8") as f:
        for row in train:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")

    with eval_path.open("w", encoding="utf-8") as f:
        for row in eval_set:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")

    print(f"Prepared {len(records)} samples")
    print(f"Train: {len(train)} -> {train_path}")
    print(f"Eval : {len(eval_set)} -> {eval_path}")


if __name__ == "__main__":
    main()
