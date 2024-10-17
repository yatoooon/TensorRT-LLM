import argparse
import os
from pathlib import Path

import tensorrt_llm
from tensorrt_llm import BuildConfig, build
from tensorrt_llm.executor import GenerationExecutor
from tensorrt_llm.hlapi import SamplingConfig
from tensorrt_llm.models import LLaMAForCausalLM


def read_input():
    while (True):
        input_text = input("<")
        if input_text in ("q", "quit"):
            break
        yield input_text


def parse_args():
    parser = argparse.ArgumentParser(description="Llama single model example")
    parser.add_argument(
        "--engine_dir",
        type=Path,
        required=True,
        help=
        "Directory to save and load the engine. When -c is specified, always rebuild and save to this dir. When -c is not specified, load engine when the engine_dir exists, rebuild otherwise"
    )
    parser.add_argument(
        "--hf_model_dir",
        type=str,
        required=True,
        help="Read the model data and tokenizer from this directory")
    parser.add_argument(
        "-c",
        "--clean_build",
        default=False,
        action="store_true",
        help=
        "Clean build the engine even if the engine_dir exists, be careful, this overwrites the engine_dir!!"
    )
    return parser.parse_args()


def main():
    tensorrt_llm.logger.set_level('verbose')
    args = parse_args()

    build_config = BuildConfig(max_input_len=256,
                               max_output_len=20,
                               max_batch_size=1)
    # just for fast build, not best for production
    build_config.builder_opt = 0
    build_config.plugin_config.gemm_plugin = "float16"

    if args.clean_build or not args.engine_dir.exists():
        args.engine_dir.mkdir(exist_ok=True, parents=True)
        os.makedirs(args.engine_dir, exist_ok=True)
        llama = LLaMAForCausalLM.from_hugging_face(args.hf_model_dir)
        engine = build(llama, build_config)
        engine.save(args.engine_dir)

    tokenizer_dir = args.hf_model_dir
    executor = GenerationExecutor.create(args.engine_dir, tokenizer_dir)
    sampling_config = SamplingConfig(max_new_tokens=20)
    for inp in read_input():
        output = executor.generate(inp, sampling_config=sampling_config)
        print(f">{output.text}")


main()
