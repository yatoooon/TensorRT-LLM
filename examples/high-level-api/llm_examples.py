#!/usr/bin/env python3
import asyncio
import os
from typing import List, Optional, Union

import click
import torch

from tensorrt_llm import LLM, ModelConfig
from tensorrt_llm.hlapi.llm import KvCacheConfig, SamplingConfig
from tensorrt_llm.hlapi.utils import get_device_count
from tensorrt_llm.quantization import QuantAlgo

# NOTE, Currently, the following examples are only available for LLaMA models.


@click.group()
def cli():
    pass


@click.command('run_llm_generate')
@click.option('--prompt', type=str, default="What is LLM?")
@click.option('--model_dir', type=str, help='The directory of the model.')
@click.option('--engine_dir',
              type=str,
              help='The directory of the engine.',
              default=None)
@click.option('--tp_size',
              type=int,
              default=1,
              help='The number of GPUs for Tensor Parallel.')
@click.option('--pp_size',
              type=int,
              default=1,
              help='The number of GPUs for Pipeline Parallel.')
@click.option('--prompt_is_digit',
              type=bool,
              default=False,
              help='Whether the prompt is a list of integers.')
def run_llm_generate(
    prompt: str,
    model_dir: str,
    engine_dir: Optional[str] = None,
    tp_size: int = 1,
    pp_size: int = 1,
    prompt_is_digit: bool = False,
    end_id: int = 2,
):
    ''' Running LLM with arbitrary model formats including:
        - HF model
        - TRT-LLM checkpoint
        - TRT-LLM engine

    It will dump the engine to `engine_dir` if specified.

    Args:
        prompts: A list of prompts. Each prompt can be either a string or a list of integers when tokenizer is disabled.
        model_dir: The directory of the model.
        engine_dir: The directory of the engine, if specified different than model_dir then it will save the engine to `engine_dir`.
        tp_size: The number of GPUs for Tensor Parallel.
        pp_size: The number of GPUs for Pipeline Parallel.
    '''

    config = ModelConfig(model_dir)
    # Avoid the tp_size and pp_size setting override the ones loaded from built engine
    if tp_size > 1: config.parallel_config.tp_size = tp_size
    if pp_size > 1: config.parallel_config.pp_size = pp_size

    if get_device_count() < config.parallel_config.world_size:
        print(
            "Skip the example for TP!!! Since the number of GPUs is less than required"
        )
        return
    if config.parallel_config.world_size > 1:
        print(f'Running LLM with Tensor Parallel on {tp_size} GPUs.')

    llm = LLM(config)

    if engine_dir and os.path.abspath(model_dir) != os.path.abspath(engine_dir):
        print(f"Saving engine to {engine_dir}...")
        llm.save(engine_dir)

    prompts = parse_prompts(prompt, prompt_is_digit)

    sampling_config = SamplingConfig(end_id=end_id,
                                     pad_id=end_id) if prompt_is_digit else None

    for output in llm.generate(prompts, sampling_config=sampling_config):
        print("OUTPUT:", output)


@click.command('run_llm_generate_async_example')
@click.option('--prompt', type=str, default="What is LLM?")
@click.option('--model_dir', type=str, help='The directory of the model.')
@click.option('--streaming',
              is_flag=True,
              help='Whether to enable streaming generation.')
@click.option('--tp_size',
              type=int,
              default=1,
              help='The number of GPUs for Tensor Parallel.')
@click.option('--pp_size',
              type=int,
              default=1,
              help='The number of GPUs for Pipeline Parallel.')
def run_llm_generate_async_example(prompt: str,
                                   model_dir: str,
                                   streaming: bool = False,
                                   tp_size: int = 1,
                                   pp_size: int = 1):
    ''' Running LLM generation asynchronously. '''

    if get_device_count() < tp_size:
        print(
            "Skip the example for TP!!! Since the number of GPUs is less than required"
        )
        return
    if tp_size > 1:
        print(f'Running LLM with Tensor Parallel on {tp_size} GPUs.')

    config = ModelConfig(model_dir)
    # Avoid the tp_size and pp_size setting override the ones loaded from built engine
    if tp_size > 1: config.parallel_config.tp_size = tp_size
    if pp_size > 1: config.parallel_config.pp_size = pp_size

    llm = LLM(config,
              kv_cache_config=KvCacheConfig(free_gpu_memory_fraction=0.4))
    prompts = parse_prompts(prompt, False)

    async def task(prompt: str):
        outputs = []
        async for output in llm.generate_async(prompt, streaming=streaming):
            outputs.append(output.text)
        print(' '.join(outputs))

    async def main():
        tasks = [task(prompt) for prompt in prompts]
        await asyncio.gather(*tasks)

    asyncio.run(main())


@click.command('run_llm_with_quantization')
@click.option('--prompt', type=str, default="What is LLM?")
@click.option('--model_dir', type=str, help='The directory of the model.')
@click.option('--quant_type',
              type=str,
              default='int4_awq',
              help='The quantization type.')
def run_llm_with_quantization(prompt: str, model_dir: str, quant_type: str):
    ''' Running LLM with quantization.
    quant_type could be 'int4_awq' or 'fp8'.
    '''

    major, minor = torch.cuda.get_device_capability()
    if not (major >= 8):
        print("Quantization currently only supported on post Ampere")
        return

    if 'fp8' in quant_type:
        if not (major > 8):
            print("Hopper GPUs are required for fp8 quantization")
            return

    config = ModelConfig(model_dir)
    if quant_type == 'int4_awq':
        config.quant_config.quant_algo = QuantAlgo.W4A16_AWQ
    else:
        config.quant_config.quant_algo = QuantAlgo.FP8
        config.quant_config.kv_cache_quant_algo = QuantAlgo.FP8
        config.quant_config.exclude_modules = ["lm_head"]

    llm = LLM(config)
    prompts = parse_prompts(prompt, False)

    for output in llm.generate(prompts):
        print(output)


@click.command('run_llm_with_async_future')
@click.option('--prompt', type=str, default="What is LLM?")
@click.option('--model_dir', type=str, help='The directory of the model.')
def run_llm_with_async_future(prompt: str, model_dir: str):
    config = ModelConfig(model_dir)
    llm = LLM(config,
              kv_cache_config=KvCacheConfig(free_gpu_memory_fraction=0.4))

    prompts = parse_prompts(prompt)
    # The result of generate() is similar to a Future, it won't block the main thread, call .result() to explicitly wait for the result
    for generation in llm.generate_async(prompts):
        # .result() is a blocking call, call it when you want to wait for the result
        output = generation.result()
        print(output.text)

    # Similar to .result(), there is an async version of .result(), which is .aresult(), and it works with the generate_async().
    async def task(prompt: str):
        generation = llm.generate_async(prompt, streaming=False)
        output = await generation.aresult()
        print(output.text)

    async def main():
        tasks = [task(prompt) for prompt in prompts]
        await asyncio.gather(*tasks)

    asyncio.run(main())


@click.command('run_llm_with_auto_parallel')
@click.option('--prompt', type=str, default="What is LLM?")
@click.option('--model_dir', type=str, help='The directory of the model.')
@click.option('--world_size',
              type=int,
              default=1,
              help='The number of GPUs for Auto Parallel.')
def run_llm_with_auto_parallel(prompt: str,
                               model_dir: str,
                               world_size: int = 1):
    ''' Running LLM with auto parallel enabled. '''
    if get_device_count() < world_size:
        print(
            "Skip the example for auto parallel!!! Since the number of GPUs is less than required"
        )
        return
    if world_size > 1:
        print(f'Running LLM with Auto Parallel on {world_size} GPUs.')

    config = ModelConfig(model_dir)
    config.parallel_config.auto_parallel = True
    config.parallel_config.world_size = world_size

    llm = LLM(config)
    prompts = parse_prompts(prompt)

    for output in llm.generate(prompts):
        print(output)


def parse_prompts(prompt: str, is_digit: bool = False) -> Union[str, List[int]]:
    ''' Process a single prompt. '''
    if is_digit:
        return [[int(i) for i in prompt.split()]]
    else:
        return [prompt]


if __name__ == '__main__':
    cli.add_command(run_llm_generate)
    cli.add_command(run_llm_generate_async_example)
    cli.add_command(run_llm_with_quantization)
    cli.add_command(run_llm_with_async_future)
    cli.add_command(run_llm_with_auto_parallel)
    cli()
