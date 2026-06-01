#!/usr/bin/env python3
import argparse
import os
import subprocess

from peft import PeftModel
from transformers import AutoModelForCausalLM, AutoTokenizer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-model", required=True)
    parser.add_argument("--lora-dir", required=True)
    parser.add_argument("--merged-dir", required=True)
    parser.add_argument("--llama-cpp-dir", default="")
    parser.add_argument("--gguf-out", default="")
    parser.add_argument("--gguf-type", default="q4_k_m")
    args = parser.parse_args()

    model = AutoModelForCausalLM.from_pretrained(args.base_model, trust_remote_code=True, device_map="auto")
    peft_model = PeftModel.from_pretrained(model, args.lora_dir)
    merged = peft_model.merge_and_unload()
    tokenizer = AutoTokenizer.from_pretrained(args.base_model, trust_remote_code=True)

    os.makedirs(args.merged_dir, exist_ok=True)
    merged.save_pretrained(args.merged_dir)
    tokenizer.save_pretrained(args.merged_dir)
    print(f"Merged model saved to {args.merged_dir}")

    if args.llama_cpp_dir and args.gguf_out:
        script = os.path.join(args.llama_cpp_dir, "convert_hf_to_gguf.py")
        cmd = [
            "python",
            script,
            args.merged_dir,
            "--outfile",
            args.gguf_out,
            "--outtype",
            args.gguf_type,
        ]
        print("Running:", " ".join(cmd))
        subprocess.check_call(cmd)
        print(f"GGUF exported to {args.gguf_out}")


if __name__ == "__main__":
    main()
