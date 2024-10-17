# Benchmark for C++ Runtime

This document explains how to benchmark the models supported by TensorRT-LLM on a single GPU, a single node with
multiple GPUs or multiple nodes with multiple GPUs.

## Usage

### 1. Build TensorRT-LLM and benchmarking source code

Please follow the [`installation document`](../../README.md#installation) to build TensorRT-LLM.

Note that the benchmarking source code for C++ runtime is not built by default, you can use the argument `--benchmarks` in [`build_wheel.py`](source:scripts/build_wheel.py) to build the corresponding executable.

Windows users: Follow the
[`Windows installation document`](../../windows/README.md)
instead, and be sure to set DLL paths as specified in
[Extra Steps for C++ Runtime Usage](../../windows/README.md#extra-steps-for-c-runtime-usage).

### 2. Launch C++ benchmarking (Fixed BatchSize/InputLen/OutputLen)

#### Prepare TensorRT-LLM engine(s)

Before you launch C++ benchmarking, please make sure that you have already built engine(s) using TensorRT-LLM API, C++ benchmarking code cannot generate engine(s) for you.

Use `trtllm-build` to build the TRT-LLM engine. Alternatively, if you have already benchmarked Python Runtime, you can reuse the engine(s) built previously, please see that [`document`](../python/README.md).

####  Launch benchmarking

For detailed usage, you can do the following
```
cd cpp/build

# You can directly execute the binary for help information
./benchmarks/gptSessionBenchmark --help
./benchmarks/bertBenchmark --help
```

Take GPT-350M as an example for single GPU

```
./benchmarks/gptSessionBenchmark \
    --engine_dir "../../benchmarks/gpt_350m/" \
    --batch_size "1" \
    --input_output_len "60,20"

# Expected output:
# [BENCHMARK] batch_size 1 input_length 60 output_length 20 latency(ms) 40.81
```
Take GPT-175B as an example for multiple GPUs
```
mpirun -n 8 ./benchmarks/gptSessionBenchmark \
    --engine_dir "../../benchmarks/gpt_175b/" \
    --batch_size "1" \
    --input_output_len "60,20"

# Expected output:
# [BENCHMARK] batch_size 1 input_length 60 output_length 20 latency(ms) 792.14
```

If you want to obtain context and generation logits, you could build an enigne with `--gather_context_logits` and `--gather_generation_logits`, respectively. Enable `--gather_all_token_logits` will enable both of them.

If you want to get the logits, you could run gptSessionBenchmark with `--print_all_logits`. This will print a large number of logit values and has a certain impact on performance.

*Please note that the expected outputs in that document are only for reference, specific performance numbers depend on the GPU you're using.*

### 3. Launch Batch Manager benchmarking (Inflight/V1 batching)

#### Prepare dataset

Run a preprocessing script to prepare/generate dataset into a json that gptManagerBenchmark can consume later. The processed output json has *input tokens length, input token ids and output tokens length*

This tool can be used in 2 different modes of traffic generation.

##### 1 – Dataset

The tool will tokenize the words and instruct the model to generate a specified number of output tokens for a request.

```
python3 prepare_dataset.py \
    --tokenizer <path/to/tokenizer> \
    --output preprocessed_dataset.json
    dataset
    --dataset-name <name of the dataset> \
    --dataset-split <split of the dataset to use> \
    --dataset-input-key <dataset dictionary key for input> \
    --dataset-prompt-key <dataset dictionary key for prompt> \
    --dataset-output-key <dataset dictionary key for output> \
    [--num-requests 100] \
    [--max-input-len 1000] \
    [--output-len-dist 100,10]
```

For datasets that don't have prompt key, set --dataset-prompt instead.
Take [cnn_dailymail dataset](https://huggingface.co/datasets/cnn_dailymail) for example:
```
python3 prepare_dataset.py \
    --tokenizer <path/to/tokenizer> \
    --output cnn_dailymail.json
    dataset
    --dataset-name cnn_dailymail \
    --dataset-split validation \
    --dataset-config-name 3.0.0 \
    --dataset-input-key article \
    --dataset-prompt "Summarize the following article:" \
    --dataset-output-key "highlights" \
    [--num-requests 100] \
    [--max-input-len 1000] \
    [--output-len-dist 100,10]
```

##### 2 – Normal token length distribution

This mode allows the user to generate normal token length distributions with a mean and std deviation specified.
For example, setting mean=100 and std dev=10 would generate requests where 95.4% of values are in <80,120> range following the normal probability distribution. Setting std dev=0 will generate all requests with the same mean number of tokens.

```
python prepare_dataset.py \
  --output token-norm-dist.json \
  --tokenizer <path/to/tokenizer> \
   token-norm-dist \
   --num-requests 100 \
   --input-mean 100 --input-stdev 10 \
   --output-mean 15 --output-stdev 0
```

For `tokenizer`, specifying the path to the local tokenizer that have already been downloaded, or simply the name of the tokenizer from HuggingFace like `meta-llama/Llama-2-7b` will both work. The tokenizer will be downloaded automatically for the latter case.


#### Prepare TensorRT-LLM engines
Please make sure that the engines are built with argument `--use_inflight_batching` and `--remove_input_padding` if you'd like to benchmark inflight batching, for more details, please see the document in TensorRT-LLM examples.

#### Launch benchmarking

For detailed usage, you can do the following
```
cd cpp/build

# You can directly execute the binary for help information
./benchmarks/gptManagerBenchmark --help
```

Take GPT-350M as an example for single GPU V1 batching
```
./benchmarks/gptManagerBenchmark \
    --engine_dir ../../examples/gpt/trt_engine/gpt2/fp16/1-gpu/ \
    --type V1 \
    --request_rate 10 \
    --dataset ../../benchmarks/cpp/preprocessed_dataset.json
    --max_num_samples 500
```

Take GPT-350M as an example for 2-GPU inflight batching
```
mpirun -n 2 ./benchmarks/gptManagerBenchmark \
    --engine_dir ../../examples/gpt/trt_engine/gpt2-ib/fp16/2-gpu/ \
    --type IFB \
    --request_rate 10 \
    --dataset ../../benchmarks/cpp/preprocessed_dataset.json
    --max_num_samples 500
```

`gptManagerBenchmark` can also be used with the high-level C++ API defined by the `executor::Executor` class (see `cpp/include/tensorrt_llm/executor/executor.h`). This can be done by passing the argument `--api executor`. Note that the Executor class is still under development and currently does not support models with tp or pp > 1.

#### Emulated static batching

To emulate `gptSessionBenchmark` static batching, you can use `gptManagerBenchmark` with the `--static_emulated_batch_size` and `--static_emulated-timeout` arguments.
Given a `static_emulated_batch_size` of `n` the server will wait for `n` requests to arrive before submitting them to the batch manager at once. If the `static_emulated_timeout` (in ms) is reached before `n` requests are collected, the batch will be submitted prematurely with the current request count. New batches will only be submitted once the previous batch has been processed comepletely.

`gptSessionBenchmark` uses fixed input/output lengths for benchmarking. A similar dataset for `gptManagerBenchmark` can be generated with the preprocessing script, e.g.
```
 python prepare_dataset.py \
  --output tokens-fixed-lengths.json \
  --tokenizer <path/to/tokenizer> \
   token-norm-dist \
   --num-requests 128 \
   --input-mean 60 --input-stdev 0 \
   --output-mean 20 --output-stdev 0
```

Take GPT-350M as an example for single GPU with static batching
```
./benchmarks/gptManagerBenchmark \
    --engine_dir ../../examples/gpt/trt_engine/gpt2/fp16/1-gpu/ \
    --type IFB \
    --request-rate -1 \
    --static_emulated_batch_size 32 \
    --static_emulated_timeout 100 \
    --dataset ../../benchmarks/cpp/tokens-fixed-lengths.json
```

#### Benchmarking LoRA

Using either of the `prepare_dataset.py` methods above, add `--rand-task-id <start-id> <end-id>` to the command. This will add a random `task_id` from `<start-id>` to `<end-id>` inclusive.
You can then use `utils/generate_rand_loras.py` to generate random LoRA weights for benchmarking purposes. `utils/generate_rand_loras.py` takes an example LoRA for the model you are benchmarking.
Then you can run `gptManagerBenchmark` with `--type IFB` and `--lora_dir /path/to/utils/generate_rand_loras/output`

End-to-end LoRA benchmarking script

```
git-lfs clone https://huggingface.co/meta-llama/Llama-2-13b-hf
git-lfs clone https://huggingface.co/hfl/chinese-llama-2-lora-13b

MODEL_CHECKPOINT=Llama-2-13b-hf
CONVERTED_CHECKPOINT=Llama-2-13b-hf-ckpt
TOKENIZER=Llama-2-13b-hf
LORA_ENGINE=Llama-2-13b-hf-engine

DTYPE=float16
TP=2
PP=1
MAX_LEN=1024
MAX_BATCH=32
MAX_LORA_RANK=32
NUM_LORA_MODS=7

SOURCE_LORA=chinese-llama-2-lora-13b
CPP_LORA=chinese-llama-2-lora-13b-cpp

EG_DIR=/tmp/lora-eg

# Build lora enabled engine
python examples/llama/convert_checkpoint.py --model_dir ${MODEL_CHECKPOINT} \
                              --output_dir ${CONVERTED_CHECKPOINT} \
                              --dtype ${DTYPE} \
                              --tp_size ${TP} \
                              --pp_size 1

${HOME}/.local/bin/trtllm-build \
    --checkpoint_dir ${CONVERTED_CHECKPOINT} \
    --output_dir ${LORA_ENGINE} \
    --max_batch_size ${MAX_BATCH} \
    --max_input_len $MAX_LEN \
    --max_output_len $MAX_LEN \
    --gemm_plugin float16 \
    --lora_plugin float16 \
    --use_paged_context_fmha enable \
    --lora_target_modules attn_qkv \
    --max_lora_rank ${MAX_LORA_RANK}

NUM_LORAS=(8 16 24 32 64 128 256)
NUM_REQUESTS=1024

# Convert LoRA to cpp format
python examples/hf_lora_convert.py \
    -i $SOURCE_LORA \
    --storage-type $DTYPE \
    -o $CPP_LORA

# Prepare datasets
mkdir -p $EG_DIR/data

# Prepare dataset without lora_task_id
python benchmarks/cpp/prepare_dataset.py \
    --output "${EG_DIR}/data/token-norm-dist.json" \
    --request-rate -1 \
    --time-delay-dist constant \
    --tokenizer $TOKENIZER \
    token-norm-dist \
    --num-requests $NUM_REQUESTS \
    --input-mean 256 --input-stdev 16 --output-mean 128 --output-stdev 24

# Prepare dataset with lora_task_ids from 0 - $nloras
for nloras in ${NUM_LORAS[@]}; do
    python benchmarks/cpp/prepare_dataset.py \
        --output "${EG_DIR}/data/token-norm-dist-lora-${nloras}.json" \
        --request-rate -1 \
        --time-delay-dist constant \
        --rand-task-id 0 $(( $nloras - 1 )) \
        --tokenizer $TOKENIZER \
        token-norm-dist \
        --num-requests $NUM_REQUESTS \
        --input-mean 256 --input-stdev 16 --output-mean 128 --output-stdev 24
done

# Generate random lora weights for 256 adapters
python benchmarks/cpp/utils/generate_rand_loras.py ${CPP_LORA} ${EG_DIR}/loras 256

# perform benchmarking

# First run inference without LoRAs
mkdir -p ${EG_DIR}/log-base-lora
mpirun -n ${TP} --output-filename ${EG_DIR}/log-base-lora \
    cpp/build_Debug/benchmarks/gptManagerBenchmark \
    --engine_dir $LORA_ENGINE \
    --type IFB \
    --dataset "${EG_DIR}/data/token-norm-dist.json" \
    --lora_host_cache_bytes 8589934592 \
    --lora_num_device_mod_layers $(( 32 * $NUM_LAYERS * $NUM_LORA_MODS * $MAX_LORA_RANK )) \
    --kv_cache_free_gpu_mem_fraction 0.80 \
    --log_level info \
    --eos_id ${EOS_ID}

# Now run inference with various numbers or loras
# The host cache is set large enough to hold all the LoRAs in lora_dir
# GPU cache is set to hold 32 LoRAs
# This benchmark will preload all the LoRAs into the host cache
# We run inference on a range of active LoRAs exercising different cache miss rates.
for nloras in ${NUM_LORAS[@]}; do
    mkdir -p ${EG_DIR}/log-lora-${nloras}
    mpirun -n ${TP} --output-filename "${EG_DIR}/log-lora-${nloras}" \
        cpp/build_Debug/benchmarks/gptManagerBenchmark \
        --engine_dir $LORA_ENGINE \
        --type IFB \
        --dataset "${EG_DIR}/data/token-norm-dist-lora-${nloras}.json" \
        --lora_host_cache_bytes 8589934592 \
        --lora_num_device_mod_layers $(( 32 * $NUM_LAYERS * $NUM_LORA_MODS * $MAX_LORA_RANK )) \
        --kv_cache_free_gpu_mem_fraction 0.80 \
        --log_level info \
        --eos_id ${EOS_ID} \
        --lora_dir ${EG_DIR}/loras
done
```
