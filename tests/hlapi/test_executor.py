import os as _os
import sys as _sys
import unittest
from pathlib import Path

import pytest
import torch
from transformers import AutoTokenizer

from tensorrt_llm._utils import mpi_world_size
from tensorrt_llm.executor import (GenerationExecutor, GenerationExecutorWorker,
                                   GenerationRequest, SamplingConfig)
from tensorrt_llm.hlapi.llm import LLM, ModelConfig

_sys.path.append(_os.path.join(_os.path.dirname(__file__), '..'))
from utils.cpp_paths import *  # noqa
from utils.llm_data import llm_models_root

WORLD_SIZE = mpi_world_size()


@pytest.fixture(scope="module")
def llama_7b_path(engine_path: Path) -> Path:
    path = engine_path / "llama7b"

    if not path.exists():
        config = ModelConfig(str(llm_models_root() /
                                 "llama-models/llama-7b-hf"))
        # TODO[chunweiy]: switch to executor backend
        llm = LLM(config, enable_executor=False)
        llm.save(str(path))

    return path


@pytest.fixture(scope="module")
def llama_7b_bs2_path(engine_path: Path) -> Path:
    path = engine_path / "llama7b_bs2"

    if not path.exists():
        config = ModelConfig(str(llm_models_root() /
                                 "llama-models/llama-7b-hf"),
                             max_beam_width=2)
        # TODO[chunweiy]: switch to executor backend
        llm = LLM(config, enable_executor=False)
        llm.save(str(path))

    return path


@pytest.fixture(scope="module")
def llama_7b_tp2_path(engine_path: Path) -> Path:
    path = engine_path / "llama7b-tp2"

    if not path.exists():
        config = ModelConfig(str(llm_models_root() /
                                 "llama-models/llama-7b-hf"))
        config.parallel_config.tp_size = 2
        # TODO[chunweiy]: switch to executor backend
        llm = LLM(config, enable_executor=False)
        llm.save(str(path))

    return path


@pytest.mark.parametrize("use_executor_bindings", [False, True])
@pytest.mark.skipif(WORLD_SIZE != 1, reason="Must run on single MPI rank")
def test_generation_bs2(use_executor_bindings: bool, llama_7b_bs2_path: Path):
    tokenizer = llama_7b_bs2_path
    prompt = "A B C D"
    max_new_tokens = 4

    with GenerationExecutor.create(
            llama_7b_bs2_path,
            tokenizer,
            max_beam_width=2,
            use_executor_bindings=use_executor_bindings) as executor:
        result = executor.generate(prompt,
                                   sampling_config=SamplingConfig(
                                       max_new_tokens=max_new_tokens,
                                       beam_width=2))
        assert result.text[0] == "<s> A B C D E F G H"
        assert result.text[1] == "<s> A B C D E F G I"


@pytest.mark.parametrize("use_executor_bindings", [False, True])
@pytest.mark.skipif(WORLD_SIZE != 1, reason="Must run on single MPI rank")
def test_sync_generation(use_executor_bindings: bool, llama_7b_path: Path):
    tokenizer = llama_7b_path
    prompt = "A B C D"
    expected_output = " E F G H"
    expected_long_output = " E F G H I J K L"
    split_output = ["E", " F", " G", " H", " I", " J", " K", " L"]
    sampling_config0 = SamplingConfig(max_new_tokens=4)
    sampling_config1 = SamplingConfig(max_new_tokens=8)
    with GenerationExecutor.create(
            llama_7b_path, tokenizer,
            use_executor_bindings=use_executor_bindings) as executor:
        # Simple generations (synchronous)
        result = executor.generate(prompt, sampling_config=sampling_config0)
        assert result.text == "<s> " + prompt + expected_output

        results = executor.generate(
            [prompt, prompt],
            sampling_config=[sampling_config0, sampling_config1])
        for result, expected in zip(results,
                                    (expected_output, expected_long_output)):
            assert result.text == "<s> " + prompt + expected

        # Simple generations (asynchronous)
        #
        # Iterate the partial results when streaming
        future = executor.generate_async(prompt,
                                         streaming=True,
                                         sampling_config=sampling_config0)
        for idx, partial_result in enumerate(future):
            assert partial_result.text_diff == split_output[idx]

        # Iterate the partial results when streaming
        # Streaming results in nested loop
        futures = executor.generate_async(
            [prompt, prompt],
            streaming=True,
            sampling_config=[sampling_config0, sampling_config1])
        for future in futures:
            for idx, partial_result in enumerate(future):
                assert partial_result.text_diff == split_output[idx]

        # Low-level api with .submit
        # Submit a batch of requests
        tokenizer = AutoTokenizer.from_pretrained("gpt2")
        futures = []
        for _ in range(5):
            futures.append(
                executor.submit(
                    GenerationRequest(
                        prompt,
                        tokenizer=AutoTokenizer.from_pretrained(llama_7b_path),
                        sampling_config=sampling_config0)))

        for future in executor.wait_first_completed(futures):
            assert future.done
            assert future.result().text == "".join(split_output[:4])


@pytest.mark.skipif(torch.cuda.device_count() < 2 or WORLD_SIZE != 2,
                    reason="Must run on 2 MPI ranks with at least 2 GPUs")
def test_sync_generation_tp_all_nodes(llama_7b_tp2_path: Path):
    prompt = "deep learning"
    sampling_config = SamplingConfig(max_new_tokens=4)

    # Normal execution, all nodes live
    executor = GenerationExecutorWorker(llama_7b_tp2_path, llama_7b_tp2_path)
    result = executor.generate(prompt, sampling_config=sampling_config)
    assert result.text == "<s> deep learning, neural network,"
    executor.shutdown()


@pytest.mark.parametrize("use_executor_bindings", [False, True])
@pytest.mark.skipif(torch.cuda.device_count() < 2 or WORLD_SIZE != 2,
                    reason="Must run on 2 MPI ranks with at least 2 GPUs")
def test_sync_generation_tp_main_node_only(use_executor_bindings: bool,
                                           llama_7b_tp2_path: Path):
    prompt = "deep learning"
    sampling_config = SamplingConfig(max_new_tokens=4)

    with GenerationExecutor.create(
            llama_7b_tp2_path,
            llama_7b_tp2_path,
            use_executor_bindings=use_executor_bindings) as executor:

        executor.block_subordinates()
        # from now on, only rank0 lives in the with statement
        # other nodes wait at the "end" of the with statement

        result = executor.generate(prompt, sampling_config=sampling_config)
        assert result.text == "<s> deep learning, neural network,"


@pytest.mark.parametrize("use_executor_bindings", [False, True])
@pytest.mark.skipif(torch.cuda.device_count() < 2 or WORLD_SIZE != 1,
                    reason="Must run on 1 MPI rank with at least 2 GPUs")
def test_sync_generation_tp_inner(use_executor_bindings: bool,
                                  llama_7b_tp2_path: Path):
    prompt = "deep learning"
    tp_size = 2
    sampling_config = SamplingConfig(max_new_tokens=4)

    executor = GenerationExecutor.create(
        llama_7b_tp2_path,
        llama_7b_tp2_path,
        model_world_size=tp_size,
        use_executor_bindings=use_executor_bindings)
    result = executor.generate(prompt, sampling_config=sampling_config)
    assert result.text == "<s> deep learning, neural network,"
    executor.shutdown()


if __name__ == "__main__":
    unittest.main()
