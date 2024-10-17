import argparse
import os
from pathlib import Path

import tensorrt_llm
from tensorrt_llm import BuildConfig, build
from tensorrt_llm.executor import GenerationExecutor
from tensorrt_llm.hlapi import SamplingConfig
from tensorrt_llm.models import LLaMAForCausalLM
from tensorrt_llm.models.modeling_utils import QuantConfig
from tensorrt_llm.quantization import QuantAlgo


def read_input():
    while (True):
        input_text = input("<")
        if input_text in ("q", "quit"):
            break
        yield input_text


def parse_args():
    parser = argparse.ArgumentParser(description="Llama single model example")
    parser.add_argument(
        "--cache_dir",
        type=str,
        required=True,
        help=
        "Directory to save and load the engine and checkpoint. When -c is specified, always rebuild and save to this dir. When -c is not specified, load engine when the engine_dir exists, rebuild otherwise"
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
        "Clean build the engine even if the cache dir exists, be careful, this overwrites the cache dir!!"
    )
    return parser.parse_args()


def main():
    tensorrt_llm.logger.set_level('verbose')
    args = parse_args()
    tokenizer_dir = args.hf_model_dir
    max_batch_size, max_isl, max_osl = 1, 256, 20
    build_config = BuildConfig(max_input_len=max_isl,
                               max_output_len=max_osl,
                               max_batch_size=max_batch_size)
    cache_dir = Path(args.cache_dir)
    checkpoint_dir = cache_dir / "trtllm_checkpoint"
    engine_dir = cache_dir / "trtllm_engine"

    if args.clean_build or not cache_dir.exists():
        os.makedirs(cache_dir, exist_ok=True)
        quant_config = QuantConfig()
        quant_config.quant_algo = QuantAlgo.W4A16_AWQ
        if not checkpoint_dir.exists():
            LLaMAForCausalLM.quantize(args.hf_model_dir,
                                      checkpoint_dir,
                                      quant_config=quant_config,
                                      calib_batches=1)
        llama = LLaMAForCausalLM.from_checkpoint(checkpoint_dir)
        engine = build(llama, build_config)
        engine.save(engine_dir)

    executor = GenerationExecutor.create(engine_dir, tokenizer_dir)

    sampling_config = SamplingConfig(max_new_tokens=20)
    for inp in read_input():
        output = executor.generate(inp, sampling_config=sampling_config)
        print(f">{output.text}")


main()
