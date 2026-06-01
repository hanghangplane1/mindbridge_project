# MindCare LoRA Pipeline

This folder contains end-to-end scripts for training the counselor LoRA model.

## 1) Install dependencies

```bash
pip install -r requirements.txt
```

## 2) Prepare training data

```bash
python prepare_data.py \
  --input /home/hang/work/PsyQA/PsyQA_alpaca_labelled.json \
  --output-dir ./data
```

## 3) Train QLoRA

```bash
python train_lora.py \
  --model Qwen/Qwen2.5-7B-Chat \
  --train-file ./data/train_data.jsonl \
  --eval-file ./data/eval_data.jsonl \
  --output-dir ./lora_output
```

## 4) Merge LoRA and export GGUF

```bash
python merge_and_export.py \
  --base-model Qwen/Qwen2.5-7B-Chat \
  --lora-dir ./lora_output \
  --merged-dir ./merged_model \
  --llama-cpp-dir /path/to/llama.cpp \
  --gguf-out ./mindcare-counselor-q4_k_m.gguf
```

## 5) Import into Ollama

```bash
ollama create mindcare-counselor -f Modelfile
```

Then run counselor agent with:

```bash
--model mindcare-counselor:latest
```
