# Phi

This document explains how to build the [phi-2](https://huggingface.co/microsoft/phi-2), [Phi-3-mini-4k-instruct](https://huggingface.co/microsoft/Phi-3-mini-4k-instruct) and [Phi-3-mini-128k-instruct](https://huggingface.co/microsoft/Phi-3-mini-128k-instruct) models using TensorRT-LLM and run on a single GPU.

- [Phi](#phi)
  - [Overview](#overview)
  - [Support Matrix](#support-matrix)
  - [Usage](#usage)
    - [1. Convert weights from HF Transformers to TensorRT-LLM format](#1-convert-weights-from-hf-transformers-to-tensorrt-llm-format)
    - [2. Build TensorRT engine(s)](#2-build-tensorrt-engines)
      - [Fused MultiHead Attention (FMHA)](#fused-multihead-attention-fmha)
    - [3. Summarization using the Phi model](#3-summarization-using-the-phi-model)

## Overview

The TensorRT-LLM Phi implementation can be found in [`tensorrt_llm/models/phi/model.py`](../../tensorrt_llm/models/phi/model.py) and [`tensorrt_llm/models/phi3/model.py`](../../tensorrt_llm/models/phi3/model.py). The TensorRT-LLM Phi example code is located in [`examples/phi`](./). There is one file:

* [`convert_checkpoint.py`](./convert_checkpoint.py) to convert a checkpoint from the [HuggingFace (HF) Transformers](https://github.com/huggingface/transformers) format to the TensorRT-LLM format

In addition, there are two shared files in the parent folder [`examples`](../) for inference and evaluation:

* [`../run.py`](../run.py) to run the inference on an input text;
* [`../summarize.py`](../summarize.py) to summarize the articles in the [cnn_dailymail](https://huggingface.co/datasets/cnn_dailymail) dataset.

## Support Matrix
  * FP16
  * BF16
  * Tensor Parallel
  ## Support Matrix

|    Model Name    | FP16  | BF16  |  TP   |
| :--------------: | :---: | :---: | :---: |
|    phi-2    |   Y   |   Y    |   Y   |
| Phi-3-mini-4k-instruct    |   Y   |   Y   |     |
| Phi-3-mini-128k-instruct  |   Y   |   Y   |    |

* Model Name: the name of the model, the same as the name on HuggingFace
* TP: Tensor Parallel

## Usage

### 1. Convert weights from HF Transformers to TensorRT-LLM format

Please install required packages first:

```bash
pip install -r requirements.txt
```

```bash
export MODEL_TYPE="phi-2" # or Phi-3-mini-4k-instruct, Phi-3-mini-128k-instruct
python ./convert_checkpoint.py --model_type ${MODEL_TYPE} \
                    --model_dir "microsoft/${MODEL_TYPE}" \
                    --output_dir ./phi-checkpoint \
                    --dtype float16
```

### 2. Build TensorRT engine(s)

TensorRT-LLM builds TensorRT engine(s) using a HF checkpoint. If no checkpoint directory is specified, TensorRT-LLM will build engine(s) using dummy weights.

Examples of build invocations:

```bash
# Build a float16 engine using a single GPU and HF weights.
# Enable several TensorRT-LLM plugins to increase runtime performance. It also helps with build time.
# --tp_size and --pp_size are the model shard size
trtllm-build \
    --checkpoint_dir ./phi-checkpoint \
    --output_dir ./phi-engine \
    --gemm_plugin float16 \
    --max_batch_size 8 \
    --max_input_len 1024 \
    --max_output_len 1024 \
    --tp_size 1 \
    --pp_size 1
```

#### Fused MultiHead Attention (FMHA)

You can enable the FMHA kernels for phi by adding `--context_fmha enable` to the invocation of `trtllm-build`. Note that it is disabled by default because of possible accuracy issues due to the use of Flash Attention.

If you find that the default fp16 accumulation (`--context_fmha enable`) cannot meet the requirement, you can try to enable fp32 accumulation by adding `--context_fmha_fp32_acc enable`. However, it is expected to see performance drop.

Note `--context_fmha enable` / `--context_fmha_fp32_acc enable` has to be used together with `--gpt_attention_plugin float16`.

### 3. Summarization using the Phi model

The following section describes how to run a TensorRT-LLM Phi model to summarize the articles from the [cnn_dailymail](https://huggingface.co/datasets/cnn_dailymail) dataset. For each summary, the script can compute the [ROUGE](https://en.wikipedia.org/wiki/ROUGE_(metric)) scores and use the `ROUGE-1` score to validate the implementation.
The script can also perform the same summarization using the HF Phi model.

As previously explained, the first step is to build the TensorRT engine as described above using HF weights. You also have to install the requirements:

```bash
pip install -r requirements.txt
```

The summarization can be done using the [`../summarize.py`](../summarize.py) script as follows:

```bash
# Run the summarization task using a TensorRT-LLM model and a single GPU.
python3 ../summarize.py --engine_dir ./phi-engine \
                        --hf_model_dir "microsoft/$(MODEL_TYPE)" \
                        --batch_size 1 \
                        --test_trt_llm \
                        --test_hf \
                        --data_type fp16 \
                        --check_accuracy \
                        --tensorrt_llm_rouge1_threshold=20

# Run the summarization task using a TensorRT-LLM model and 2-way tensor parallelism.
mpirun -n 2 --allow-run-as-root                             \
python3 ../summarize.py --engine_dir ./phi-engine-tp2  \
                        --hf_model_dir "microsoft/$(MODEL_TYPE)"    \
                        --batch_size 1                      \
                        --test_hf                           \
                        --test_trt_llm                      \
                        --data_type fp16                    \
                        --check_accuracy                    \
                        --tensorrt_llm_rouge1_threshold 20
```
