# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import argparse
import multiprocessing as mp
import os
import time
from collections import OrderedDict

# isort: off
import torch
import tensorrt as trt
# isort: on

from allowed_configs import (get_allowed_models, get_build_config,
                             get_model_family)
from base_benchmark import get_engine_name, serialize_engine

import tensorrt_llm
from tensorrt_llm._utils import str_dtype_to_trt
from tensorrt_llm.builder import Builder
from tensorrt_llm.functional import LayerNormPositionType, LayerNormType
from tensorrt_llm.logger import logger
from tensorrt_llm.mapping import Mapping
from tensorrt_llm.models import PretrainedConfig
from tensorrt_llm.models.modeling_utils import QuantConfig, optimize_model
from tensorrt_llm.network import net_guard
from tensorrt_llm.plugin.plugin import ContextFMHAType
from tensorrt_llm.quantization import QuantAlgo
from tensorrt_llm.quantization.quantize import quantize

WEIGHT_STREAMING_DISABLED_VAL = "1.0"


def parse_arguments():
    parser = argparse.ArgumentParser(description='Build TensorRT-LLM models.')
    parser.add_argument('-m',
                        '--model',
                        type=str,
                        required=True,
                        choices=get_allowed_models(),
                        help='Specify model you want to build.')
    parser.add_argument(
        '--mode',
        type=str,
        default="plugin",
        choices=['ootb', 'plugin', 'plugin-ifb', 'ootb-except-mha'],
        help=
        ('Choose mode between ootb/plugin/ootb-except-mha. '
         '\"ootb\" means the engines will be built without any plugins, '
         '\"plugin\" means the engines will be built with tuned recipe of using plugins.'
         '\"plugin-ifb\" will include additional options required for inflight batching.'
         '\"ootb-except-mha\" means the engines will be built with only attention plugins.'
         ))

    parser.add_argument(
        '--dtype',
        type=str,
        default='float16',
        choices=['float16', 'bfloat16', 'float32'],
        help='Choose data type between float16/bfloat16/float32.')
    parser.add_argument(
        '--quantization',
        type=str,
        default=None,
        choices=[
            'fp8', 'fp8_gemm', 'fp8_kv_cache', 'int8_sq_per_tensor',
            'int8_sq_per_token_channel', 'int8_weight_only', 'int4_weight_only',
            'int4_weight_only_awq', 'int4_weight_only_gptq'
        ],
        help="Optimize the model with specified quantization recipe")

    parser.add_argument(
        '--input_timing_cache',
        type=str,
        default=None,
        help=
        'The path to read timing cache, will be ignored if the file does not exist'
    )
    parser.add_argument('--output_timing_cache',
                        type=str,
                        default='model.cache',
                        help='The path to write timing cache')

    parser.add_argument(
        '--profiling_verbosity',
        type=str,
        default='layer_names_only',
        choices=['layer_names_only', 'detailed', 'none'],
        help=
        'The profiling verbosity for the generated TRT engine. Set to detailed can inspect tactic choices and kernel parameters.'
    )
    parser.add_argument(
        '--log_level',
        type=str,
        default="error",
        choices=['verbose', 'info', 'warning', 'error', 'internal_error'],
        help=
        'Choose log level between verbose/info/warning/error/internal_error.')

    parser.add_argument(
        '--output_dir',
        type=str,
        required=True,
        help='TensorRT engines will be saved to the specified path.')

    parser.add_argument(
        '--max_beam_width',
        type=int,
        default=None,
        help=
        ('If this option is specified, it will override the max beam width of '
         'TRT engines to the specified value instead of using pre-defined one'))
    parser.add_argument(
        '--max_input_len',
        type=int,
        default=None,
        help=
        ('If this option is specified, it will override the max input len of '
         'TRT engines to the specified value instead of using pre-defined one'))
    parser.add_argument(
        '--max_output_len',
        type=int,
        default=None,
        help=
        ('If this option is specified, it will override the max output len of '
         'TRT engines to the specified value instead of using pre-defined one'))
    parser.add_argument(
        '--max_batch_size',
        type=int,
        default=None,
        help=
        ('If this option is specified, it will override the max batch size of '
         'TRT engines to the specified value instead of using pre-defined one'))
    parser.add_argument('--force_num_layer_1',
                        default=False,
                        action='store_true',
                        help='Quick sanity check with num_layer=1.')
    parser.add_argument('--serial_build',
                        default=False,
                        action='store_true',
                        help="Build engines serially")
    parser.add_argument('--strongly_typed',
                        default=False,
                        action='store_true',
                        help='This option will reduce the building time.')
    parser.add_argument(
        '--multiple_profiles',
        default=False,
        action='store_true',
        help=
        'This option will benefit performance, but will increase the engine build time.'
    )

    parser.add_argument(
        '--weight_streaming',
        default=False,
        action='store_true',
        help=
        'Specify whether offloading weights to CPU and streaming loading at runtime.',
    )

    parser.add_argument(
        '--rank',
        type=int,
        default=None,
        help=
        "The rank of the model to be built, only used when --serial_build is specified"
    )
    parser.add_argument(
        '--world_size',
        type=int,
        default=None,
        help=
        "The number of gpus to be used for inference, only used when --serial_build is specified"
    )
    parser.add_argument(
        '--opt_batch_size',
        type=int,
        default=None,
        help=
        "If opt_batch_size option is specified, it will override the opt batch size."
        "This flag only takes effect when `--mode=ootb` is added. For other modes, please use --opt_num_tokens to replace it."
    )

    parser.add_argument(
        '--opt_num_tokens',
        type=int,
        default=None,
        help="It equals to max_batch_size*max_beam_width by default, set this "
        "value as close as possible to the actual number of tokens on your workload. "
        "Note that this argument might be removed in the future."
        "This flag only takes effect when `--mode` is not `ootb`. For ootb mode, please use --opt_batch_size to replace it."
    )

    return parser.parse_args()


def get_quant_config(quantization: str):
    if quantization == "fp8":
        return QuantConfig(quant_algo=QuantAlgo.FP8,
                           kv_cache_quant_algo=QuantAlgo.FP8)
    elif quantization == "fp8_gemm":
        return QuantConfig(quant_algo=QuantAlgo.FP8)
    elif quantization == "fp8_kv_cache":
        return QuantConfig(kv_cache_quant_algo=QuantAlgo.FP8)
    elif quantization == "int8_sq_per_tensor":
        return QuantConfig(quant_algo=QuantAlgo.W8A8_SQ_PER_TENSOR_PLUGIN)
    elif quantization == "int8_sq_per_token_channel":
        return QuantConfig(
            quant_algo=QuantAlgo.W8A8_SQ_PER_CHANNEL_PER_TOKEN_PLUGIN)
    elif quantization == "int8_weight_only":
        return QuantConfig(quant_algo=QuantAlgo.W8A16)
    elif quantization == "int4_weight_only":
        return QuantConfig(quant_algo=QuantAlgo.W4A16)
    elif quantization == "int4_weight_only_awq":
        return QuantConfig(quant_algo=QuantAlgo.W4A16_AWQ)
    elif quantization == "int4_weight_only_gptq":
        return QuantConfig(quant_algo=QuantAlgo.W4A16_GPTQ)
    elif quantization is None:
        return QuantConfig()
    else:
        raise Exception(f"Unexpected quantization: {quantization}")


def build_gpt(args):
    build_config = get_build_config(args.model)
    if args.force_num_layer_1:
        build_config['num_layers'] = 1

    # More parameters
    if args.serial_build and args.rank is not None and args.world_size is not None:
        runtime_rank = args.rank
        world_size = args.world_size
    else:
        runtime_rank = tensorrt_llm.mpi_rank()
        world_size = tensorrt_llm.mpi_world_size()
    if not args.serial_build:
        torch.cuda.set_device(runtime_rank)

    strongly_typed = args.strongly_typed
    if args.quantization is not None and "fp8" in args.quantization:
        strongly_typed = True
    num_kv_heads = build_config['num_heads'] \
        if build_config['num_kv_heads'] is None else build_config['num_kv_heads']
    apply_query_key_layer_scaling = False
    max_batch_size = build_config['max_batch_size'] \
        if args.max_batch_size is None else args.max_batch_size
    max_input_len = build_config['max_input_len'] \
        if args.max_input_len is None else args.max_input_len
    max_output_len = build_config['max_output_len'] \
        if args.max_output_len is None else args.max_output_len
    max_beam_width = build_config['max_beam_width'] \
        if args.max_beam_width is None else args.max_beam_width

    opt_batch_size = build_config[
        'opt_batch_size'] if args.opt_batch_size is None else args.opt_batch_size

    opt_num_tokens = build_config[
        'opt_num_tokens'] if args.opt_num_tokens is None else args.opt_num_tokens

    if args.mode != "ootb" and opt_batch_size is not None:
        raise Exception(
            f'--opt_batch_size only used when mode is ootb. Please using --opt_num_tokens instead it.'
        )
    if args.mode == "ootb" and opt_num_tokens is not None:
        raise Exception(
            f'--opt_num_tokens does not support ootb mode. Please using --opt_batch_size instead it.'
        )

    quant_config = get_quant_config(args.quantization)
    quant_algo = quant_config.quant_algo
    kv_cache_quant_algo = quant_config.kv_cache_quant_algo
    quant_mode = quant_config.quant_mode

    is_weight_streaming = getattr(args, "weight_streaming", False)

    builder = Builder()
    builder_config_extra_kwargs = {}
    extra_items = [
        'layer_types', 'conv_kernel', 'rnn_hidden_size', 'logits_soft_cap',
        'state_size', 'use_bias'
    ]
    for item in extra_items:
        if item in build_config:
            builder_config_extra_kwargs[item] = build_config[item]
    builder_config = builder.create_builder_config(
        name=args.model,
        precision=args.dtype,
        timing_cache=args.input_timing_cache,
        profiling_verbosity=args.profiling_verbosity,
        tensor_parallel=world_size,  # TP only
        parallel_build=True,
        num_layers=build_config['num_layers'],
        num_heads=build_config['num_heads'],
        num_kv_heads=num_kv_heads,
        hidden_size=build_config['hidden_size'],
        vocab_size=build_config['vocab_size'],
        hidden_act=build_config['hidden_act'],
        max_position_embeddings=build_config['n_positions'],
        apply_query_key_layer_scaling=apply_query_key_layer_scaling,
        max_batch_size=max_batch_size,
        max_beam_width=max_beam_width,
        max_input_len=max_input_len,
        max_output_len=max_output_len,
        int8=(quant_mode.has_act_and_weight_quant()
              or quant_mode.is_int8_weight_only()),
        quant_mode=quant_mode,
        use_refit=False,
        opt_level=build_config['builder_opt'],
        strongly_typed=strongly_typed,
        weight_streaming=is_weight_streaming,
        **builder_config_extra_kwargs)
    engine_name = get_engine_name(args.model, args.dtype, world_size,
                                  runtime_rank)

    # Initialize Module
    family = get_model_family(args.model)
    if family == "gpt":
        if build_config['num_kv_heads'] is None:
            build_config['num_kv_heads'] = build_config['num_heads']
        if build_config['inter_size'] is None:
            build_config['inter_size'] = build_config['hidden_size'] * 4
        if build_config['position_embedding_type'] is None:
            build_config['position_embedding_type'] = 'learned_absolute'

        config = {
            'architecture': 'GPTForCausalLM',
            'dtype': args.dtype,
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'num_key_value_heads': build_config['num_kv_heads'],
            'hidden_size': build_config['hidden_size'],
            'intermediate_size': build_config['inter_size'],
            'norm_epsilon': 1e-05,
            'vocab_size': build_config['vocab_size'],
            'position_embedding_type': build_config['position_embedding_type'],
            'max_position_embeddings': build_config['n_positions'],
            'hidden_act': build_config['hidden_act'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128,
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size,
            },
            'bias': build_config['bias'],
            'apply_query_key_layer_scaling':
            builder_config.apply_query_key_layer_scaling,
            'rotary_pct': build_config['rotary_pct'],
            'moe_num_experts': build_config["moe_num_experts"],
            'moe_top_k': build_config["moe_top_k"],
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.GPTForCausalLM(config)

    elif family == "opt":
        config = {
            'architecture': 'OPTForCausalLM',
            'dtype': args.dtype,
            'vocab_size': build_config['vocab_size'],
            'hidden_size': build_config['hidden_size'],
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'hidden_act': build_config['hidden_act'],
            'max_position_embeddings': build_config['n_positions'],
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'use_parallel_embedding': False,
            'share_embedding_table': False,
            'embedding_sharding_dim': 0,
            'do_layer_norm_before': build_config['do_layer_norm_before'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128
            }
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.OPTForCausalLM(config)

    elif family == "llama":
        config = {
            'architecture':
            'LLaMAForCausalLM',
            'dtype':
            args.dtype,
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'num_key_value_heads':
            build_config['num_heads'] if build_config['num_kv_heads'] is None
            else build_config['num_kv_heads'],
            'hidden_size':
            build_config['hidden_size'],
            'intermediate_size':
            build_config['inter_size'],
            'vocab_size':
            build_config['vocab_size'],
            'position_embedding_type':
            'rope_gpt_neox',
            'max_position_embeddings':
            build_config['n_positions'],
            'hidden_act':
            build_config['hidden_act'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'moe_num_experts':
            build_config["moe_num_experts"],
            'moe_top_k':
            build_config["moe_top_k"],
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.LLaMAForCausalLM(config)
        tensorrt_llm_model = optimize_model(tensorrt_llm_model,
                                            use_fused_mlp=True)
    elif family == "gptj":
        config = {
            'architecture': 'GPTJForCausalLM',
            'dtype': args.dtype,
            'vocab_size': build_config['vocab_size'],
            'hidden_size': build_config['hidden_size'],
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'hidden_act': build_config['hidden_act'],
            'max_position_embeddings': build_config['n_positions'],
            'rotary_dim': build_config['rotary_dim'],
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'use_parallel_embedding': False,
            'share_embedding_table': False,
            'embedding_sharding_dim': 0,
            'do_layer_norm_before': build_config['do_layer_norm_before'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128
            }
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.GPTJForCausalLM(config)

    elif family == "gptneox":
        config = {
            'architecture':
            'GPTNeoXForCausalLM',
            'dtype':
            args.dtype,
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'hidden_size':
            build_config['hidden_size'],
            'vocab_size':
            build_config['vocab_size'],
            'position_embedding_type':
            'learned_absolute',
            'max_position_embeddings':
            build_config['n_positions'],
            'rotary_emb_base':
            10000,
            'rotary_pct':
            1.0 * build_config['rotary_dim'] * build_config['num_heads'] /
            build_config['hidden_size'],
            'hidden_act':
            build_config['hidden_act'],
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'use_parallel_embedding':
            False,
            'share_embedding_table':
            False,
            'embedding_sharding_dim':
            0,
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128,
            }
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.GPTNeoXForCausalLM(config)

    elif family == "chatglm":
        config = {
            'architecture': 'ChatGLMForCausalLM',
            'dtype': args.dtype,
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'num_key_value_heads': build_config['num_kv_heads'],
            'hidden_size': build_config['hidden_size'],
            'intermediate_size': build_config['inter_size'],
            'norm_epsilon': 1e-5,
            'vocab_size': build_config['vocab_size'],
            'position_embedding_type': 'chatglm',
            'max_position_embeddings': build_config['n_positions'],
            'hidden_act': build_config['hidden_act'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'chatglm_version': 'chatglm',
            'add_bias_linear': True,
            'add_qkv_bias': True,
            'apply_query_key_layer_scaling': False,
            'apply_residual_connection_post_layernorm': False,
            'rmsnorm': False,
            'rope_ratio': 1.0,
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.ChatGLMForCausalLM(config)

    elif family in ["chatglm2", "chatglm3"]:
        config = {
            'architecture': 'ChatGLMForCausalLM',
            'dtype': args.dtype,
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'num_key_value_heads': build_config['num_kv_heads'],
            'hidden_size': build_config['hidden_size'],
            'intermediate_size': build_config['inter_size'],
            'norm_epsilon': 1e-5,
            'vocab_size': build_config['vocab_size'],
            'position_embedding_type': 'rope_gptj',
            'max_position_embeddings': build_config['n_positions'],
            'hidden_act': build_config['hidden_act'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'chatglm_version': family,
            'add_bias_linear': False,
            'add_qkv_bias': True,
            'apply_query_key_layer_scaling': False,
            'apply_residual_connection_post_layernorm': False,
            'rmsnorm': True,
            'rope_ratio': 1.0,
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.ChatGLMForCausalLM(config)

    elif family == "bloom":
        config = {
            'architecture': 'BloomForCausalLM',
            'dtype': args.dtype,
            'vocab_size': build_config['vocab_size'],
            'hidden_size': build_config['hidden_size'],
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'hidden_act': build_config['hidden_act'],
            'max_position_embeddings': build_config['n_positions'],
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'use_parallel_embedding': (args.model == 'bloom_176b'),
            'share_embedding_table': False,
            'embedding_sharding_dim': 0,
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128
            }
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.BloomForCausalLM(config)
        tensorrt_llm_model = optimize_model(
            tensorrt_llm_model,
            use_parallel_embedding=config.use_parallel_embedding)
    elif family == "falcon":
        config = {
            'architecture':
            'FalconForCausalLM',
            'dtype':
            args.dtype,
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'num_key_value_heads':
            build_config['num_heads'] if build_config['num_kv_heads'] is None
            else build_config['num_kv_heads'],
            'hidden_size':
            build_config['hidden_size'],
            'vocab_size':
            build_config['vocab_size'],
            'position_embedding_type':
            'alibi_with_scale'
            if build_config['use_alibi'] else 'rope_gpt_neox',
            'max_position_embeddings':
            build_config['n_positions'],
            'hidden_act':
            build_config['hidden_act'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'bias':
            build_config['bias'],
            'parallel_attention':
            build_config['parallel_attention'],
            'new_decoder_architecture':
            build_config['new_decoder_architecture'],
        }
        if quant_mode.is_weight_only() and quant_mode.has_per_group_scaling():
            config['quantization'].update({
                'has_zero_point': False,
                'pre_quant_scale': True,
                'exclude_modules': [],
            })
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.FalconForCausalLM(config)

    elif family == "baichuan":
        config = {
            'architecture':
            'BaichuanForCausalLM',
            'dtype':
            args.dtype,
            'logits_dtype':
            'float32',
            'vocab_size':
            build_config['vocab_size'],
            'max_position_embeddings':
            build_config['n_positions'],
            'hidden_size':
            build_config['hidden_size'],
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'num_key_value_heads':
            build_config['num_heads'],
            'hidden_act':
            build_config['hidden_act'],
            'intermediate_size':
            build_config['inter_size'],
            'position_embedding_type':
            'alibi_with_scale' if '7b' in args.model else 'rope_gpt_neox',
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo,
                'group_size': 128
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size,
            },
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.BaichuanForCausalLM(config)

    elif family == "internlm":
        config = {
            'architecture':
            'LLaMAForCausalLM',
            'dtype':
            args.dtype,
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'num_key_value_heads':
            build_config['num_heads'] if build_config['num_kv_heads'] is None
            else build_config['num_kv_heads'],
            'hidden_size':
            build_config['hidden_size'],
            'vocab_size':
            build_config['vocab_size'],
            'position_embedding_type':
            'rope_gpt_neox',
            'max_position_embeddings':
            build_config['n_positions'],
            'hidden_act':
            build_config['hidden_act'],
            'quantization': {
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'attn_bias':
            build_config['bias'],
        }
        if quant_mode.is_weight_only():
            if 'awq' in args.quantization:
                config['quantization'].update({
                    "group_size": 128,
                    "has_zero_point": False,
                    "pre_quant_scale": True,
                    "exclude_modules": [],
                })
            elif 'gptq' in args.quantization:
                config['quantization'].update({
                    "group_size": 128,
                    "has_zero_point": True,
                    "pre_quant_scale": False,
                })
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.LLaMAForCausalLM(config)

    elif family == "qwen":
        config = {
            'architecture':
            'QWenForCausalLM',
            'dtype':
            args.dtype,
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'num_key_value_heads':
            build_config['num_heads'] if build_config['num_kv_heads'] is None
            else build_config['num_kv_heads'],
            'hidden_size':
            build_config['hidden_size'],
            'intermediate_size':
            build_config['inter_size'],
            'vocab_size':
            build_config['vocab_size'],
            'position_embedding_type':
            'rope_gpt_neox',
            'max_position_embeddings':
            build_config['n_positions'],
            'hidden_act':
            build_config['hidden_act'],
            'quantization': {
                'group_size': 128,
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'moe_num_experts':
            build_config["moe_num_experts"],
            'moe_top_k':
            build_config["moe_top_k"],
            'qwen_type':
            'qwen',
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.QWenForCausalLM(config)
    elif family == "qwen2":
        config = {
            'architecture':
            'QWenForCausalLM',
            'dtype':
            args.dtype,
            'num_hidden_layers':
            build_config['num_layers'],
            'num_attention_heads':
            build_config['num_heads'],
            'num_key_value_heads':
            build_config['num_heads'] if build_config['num_kv_heads'] is None
            else build_config['num_kv_heads'],
            'hidden_size':
            build_config['hidden_size'],
            'intermediate_size':
            build_config['inter_size'],
            'vocab_size':
            build_config['vocab_size'],
            'position_embedding_type':
            'rope_gpt_neox',
            'max_position_embeddings':
            build_config['n_positions'],
            'hidden_act':
            build_config['hidden_act'],
            'quantization': {
                'group_size': 128,
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'moe_num_experts':
            build_config["moe_num_experts"],
            'moe_top_k':
            build_config["moe_top_k"],
            'qwen_type':
            'qwen2',
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.QWenForCausalLM(config)
    elif family == "mamba":
        config = {
            'architecture': 'MambaForCausalLM',
            'dtype': args.dtype,
            'vocab_size': build_config['vocab_size'],
            'hidden_size': build_config['hidden_size'],
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'hidden_act': build_config['hidden_act'],
            'state_size': build_config['state_size'],
            'conv_kernel': build_config['conv_kernel'],
            'rnn_hidden_size': build_config['rnn_hidden_size'],
            'rms_norm': True,
            'residual_in_fp32': True,
            'pad_vocab_size_multiple': 8,
            'use_bias': build_config['use_bias'],
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.MambaForCausalLM(config)
    elif family == "recurrentgemma":
        config = {
            'architecture': 'RecurrentGemmaForCausalLM',
            'dtype': args.dtype,
            'vocab_size': build_config['vocab_size'],
            'hidden_size': build_config['hidden_size'],
            'num_hidden_layers': build_config['num_layers'],
            'num_attention_heads': build_config['num_heads'],
            'num_key_value_heads': build_config['num_kv_heads'],
            'hidden_act': build_config['hidden_act'],
            'intermediate_size': build_config['inter_size'],
            'rms_norm': True,
            'norm_epsilon': 1e-6,
            'quantization': {
                'group_size': 128,
                'quant_algo': quant_algo,
                'kv_cache_quant_algo': kv_cache_quant_algo
            },
            'mapping': {
                'world_size': world_size,
                'tp_size': world_size
            },
            'position_embedding_type': build_config['position_embedding_type'],
            'rotary_percentage': build_config['rotary_pct'],
            'max_position_embeddings': build_config['n_positions'],
            'conv_kernel': build_config['conv_kernel'],
            'state_size': build_config['state_size'],
            'layer_types': build_config['layer_types'],
            'rnn_hidden_size': build_config['rnn_hidden_size'],
            'logits_soft_cap': build_config['logits_soft_cap'],
        }
        config = PretrainedConfig.from_dict(config)
        tensorrt_llm_model = tensorrt_llm.models.RecurrentGemmaForCausalLM(
            config)

    else:
        raise Exception(f'Unexpected model: {args.model}')

    # Module -> Network
    network = builder.create_network()
    network.trt_network.name = engine_name
    network.plugin_config.to_legacy_setting()

    # Plugins
    if args.mode in ['plugin', 'plugin-ifb']:
        network.plugin_config.set_gpt_attention_plugin(dtype=args.dtype)
        network.plugin_config.set_context_fmha(ContextFMHAType.enabled)
        network.plugin_config.enable_remove_input_padding()
        network.plugin_config.set_lookup_plugin(dtype=args.dtype)
        network.plugin_config.set_moe_plugin(dtype=args.dtype)
        network.plugin_config.set_mamba_conv1d_plugin(dtype=args.dtype)

        if args.quantization is None or "fp8" not in args.quantization:
            network.plugin_config.set_gemm_plugin(dtype=args.dtype)

        # Quantization plugins.
        use_smooth_quant = quant_mode.has_act_and_weight_quant()
        use_weight_only = quant_mode.is_weight_only()
        if use_smooth_quant:
            network.plugin_config.set_smooth_quant_plugins(dtype=args.dtype)
        elif use_weight_only:
            network.plugin_config.set_weight_only_quant_matmul_plugin(
                dtype=args.dtype)
        elif family == "llama" and quant_mode.has_act_and_weight_quant():
            # RMS norm plugin for SmoothQuant
            network.plugin_config.set_rmsnorm_quantization_plugin(
                dtype=args.dtype)

        # Inflight batching
        if args.mode == 'plugin-ifb':
            network.plugin_config.enable_paged_kv_cache()
            network.plugin_config.enable_paged_state()
    elif args.mode == 'ootb-except-mha':
        network.plugin_config.set_gpt_attention_plugin(dtype=args.dtype)
        network.plugin_config.set_context_fmha(ContextFMHAType.enabled)
        network.plugin_config.enable_remove_input_padding()

    if world_size > 1:
        network.plugin_config.set_nccl_plugin(
            dtype=args.dtype,
            use_custom_all_reduce=build_config["use_custom_all_reduce"])

    if args.multiple_profiles:
        network.plugin_config.multiple_profiles = True

    with net_guard(network):
        # Prepare
        network.set_named_parameters(tensorrt_llm_model.named_parameters())

        # Forward
        print(
            f"max_batch_size: {max_batch_size}, max_input_len: {max_input_len}, max_output_len: {max_output_len}, max_beam_width: {max_beam_width}"
        )
        inputs = tensorrt_llm_model.prepare_inputs(
            max_batch_size=max_batch_size,
            max_input_len=max_input_len,
            max_seq_len=max_input_len + max_output_len,
            use_cache=True,
            max_beam_width=max_beam_width,
            opt_batch_size=opt_batch_size,
            opt_num_tokens=opt_num_tokens)

        tensorrt_llm_model(**inputs)

    if args.mode in ['plugin', 'plugin-ifb']:
        tensorrt_llm.graph_rewriting.optimize(network)

    # Network -> Engine
    start = time.time()
    engine = builder.build_engine(network, builder_config)
    assert engine is not None, f'Failed to build engine for rank {runtime_rank}'
    build_time = round(time.time() - start, 2)

    if args.output_dir is not None:
        os.makedirs(args.output_dir, exist_ok=True)
        serialize_path = os.path.join(args.output_dir, engine_name)
        serialize_engine(engine, serialize_path)
        if runtime_rank == 0:
            config_path = os.path.join(args.output_dir, 'config.json')
            builder_config.plugin_config = network.plugin_config
            builder.save_config(builder_config, config_path)
            if args.output_timing_cache:
                # Save timing cache to output_dir if not absolute path
                timing_cache_path = args.output_timing_cache if os.path.isabs(
                    args.output_timing_cache) else os.path.join(
                        args.output_dir, args.output_timing_cache)
                ok = builder.save_timing_cache(builder_config,
                                               timing_cache_path)
                if not ok:
                    logger.warning("Failed to save timing cache.")

    return engine, build_time


def build_bert(args):
    family = get_model_family(args.model)
    build_config = get_build_config(args.model)
    if args.force_num_layer_1:
        build_config['num_layers'] = 1

    # More parameters
    if args.serial_build and args.rank is not None and args.world_size is not None:
        runtime_rank = args.rank
        world_size = args.world_size
    else:
        runtime_rank = tensorrt_llm.mpi_rank()
        world_size = tensorrt_llm.mpi_world_size()
    if not args.serial_build:
        torch.cuda.set_device(runtime_rank)

    num_kv_heads = build_config['num_heads'] \
        if build_config['num_kv_heads'] is None else build_config['num_kv_heads']
    max_batch_size = build_config['max_batch_size'] \
        if args.max_batch_size is None else args.max_batch_size
    max_input_len = build_config['max_input_len'] \
        if args.max_input_len is None else args.max_input_len
    bs_range = [1, (max_batch_size + 1) // 2, max_batch_size]
    inlen_range = [1, (max_input_len + 1) // 2, max_input_len]

    is_weight_streaming = getattr(args, "weight_streaming", False)

    builder = Builder()
    builder_config = builder.create_builder_config(
        name=args.model,
        precision=args.dtype,
        timing_cache=args.input_timing_cache,
        profiling_verbosity=args.profiling_verbosity,
        tensor_parallel=world_size,  # TP only
        parallel_build=True,
        num_layers=build_config['num_layers'],
        num_heads=build_config['num_heads'],
        num_kv_heads=num_kv_heads,
        hidden_size=build_config['hidden_size'],
        vocab_size=build_config['vocab_size'],
        hidden_act=build_config['hidden_act'],
        max_position_embeddings=build_config['n_positions'],
        max_batch_size=max_batch_size,
        max_input_len=max_input_len,
        opt_level=build_config['builder_opt'],
        strongly_typed=args.strongly_typed,
        weight_streaming=is_weight_streaming,
    )
    engine_name = get_engine_name(args.model, args.dtype, world_size,
                                  runtime_rank)

    # Initialize model
    tensorrt_llm_bert = tensorrt_llm.models.BertModel(
        num_layers=build_config['num_layers'],
        num_heads=build_config['num_heads'],
        hidden_size=build_config['hidden_size'],
        vocab_size=build_config['vocab_size'],
        hidden_act=build_config['hidden_act'],
        max_position_embeddings=build_config['n_positions'],
        type_vocab_size=build_config['type_vocab_size'],
        pad_token_id=None
        if family == 'bert' else 1,  # hard code for RoBERTa here
        is_roberta=(family == 'roberta'),
        mapping=tensorrt_llm.Mapping(world_size=world_size, tp_size=world_size),
        dtype=str_dtype_to_trt(args.dtype))

    # Module -> Network
    network = builder.create_network()
    network.trt_network.name = engine_name
    network.plugin_config.to_legacy_setting()

    # Plugins
    if args.mode == 'plugin':
        network.plugin_config.set_bert_attention_plugin(dtype=args.dtype)
        network.plugin_config.set_gemm_plugin(dtype=args.dtype)
        network.plugin_config.enable_qk_half_accum()
        network.plugin_config.set_context_fmha(ContextFMHAType.enabled)
    elif args.mode == 'ootb-except-mha':
        network.plugin_config.set_bert_attention_plugin(dtype=args.dtype)
        network.plugin_config.set_context_fmha(ContextFMHAType.enabled)

    if world_size > 1:
        network.plugin_config.set_nccl_plugin(
            dtype=args.dtype,
            use_custom_all_reduce=build_config["use_custom_all_reduce"])

    with net_guard(network):
        # Prepare
        network.set_named_parameters(tensorrt_llm_bert.named_parameters())

        # Forward
        input_ids = tensorrt_llm.Tensor(
            name='input_ids',
            dtype=trt.int32,
            shape=[-1, -1],
            dim_range=OrderedDict([('batch_size', [bs_range]),
                                   ('input_len', [inlen_range])]),
        )
        input_lengths = tensorrt_llm.Tensor(name='input_lengths',
                                            dtype=trt.int32,
                                            shape=[-1],
                                            dim_range=OrderedDict([
                                                ('batch_size', [bs_range])
                                            ]))
        hidden_states = tensorrt_llm_bert(input_ids=input_ids,
                                          input_lengths=input_lengths)

        # Mark outputs
        hidden_states_dtype = str_dtype_to_trt(args.dtype)
        hidden_states.mark_output('hidden_states', hidden_states_dtype)

    # Network -> Engine
    start = time.time()
    engine = builder.build_engine(network, builder_config)
    assert engine is not None, f'Failed to build engine for rank {runtime_rank}'
    build_time = round(time.time() - start, 2)

    if args.output_dir is not None:
        if not os.path.exists(args.output_dir):
            os.makedirs(args.output_dir)
        serialize_path = os.path.join(args.output_dir, engine_name)
        serialize_engine(engine, serialize_path)
        if runtime_rank == 0:
            config_path = os.path.join(args.output_dir, 'config.json')
            builder_config.plugin_config = network.plugin_config
            builder.save_config(builder_config, config_path)
            if args.output_timing_cache:
                # Save timing cache to output_dir if not absolute path
                timing_cache_path = args.output_timing_cache if os.path.isabs(
                    args.output_timing_cache) else os.path.join(
                        args.output_dir, args.output_timing_cache)
                ok = builder.save_timing_cache(builder_config,
                                               timing_cache_path)
                if not ok:
                    logger.warning("Failed to save timing cache.")

    return engine, build_time


def enc_dec_build_helper(component, config, args):
    # More parameters
    if args.serial_build and args.rank is not None and args.world_size is not None:
        runtime_rank = args.rank
        world_size = args.world_size
    else:
        runtime_rank = tensorrt_llm.mpi_rank()
        world_size = tensorrt_llm.mpi_world_size()
    if not args.serial_build:
        torch.cuda.set_device(runtime_rank)

    family = get_model_family(args.model)
    logits_dtype = 'float32'
    n_mels = 0
    if family == 'bart':
        q_scaling = 1.0
        has_attention_qkvo_bias = True
        has_mlp_bias = True
        has_model_final_layernorm = False
        has_position_embedding = True
        has_embedding_layernorm = True
        layernorm_type = LayerNormType.LayerNorm
        relative_attention = False
        layernorm_position = LayerNormPositionType.pre_layernorm if config.get(
            'normalize_before', True) else LayerNormPositionType.post_layernorm
        rescale_before_lm_head = False
    elif family == 'whisper':
        q_scaling = 1.0
        has_position_embedding = True
        relative_attention = False
        has_embedding_layernorm = False
        has_attention_qkvo_bias = True
        has_mlp_bias = True
        has_model_final_layernorm = True
        layernorm_position = LayerNormPositionType.pre_layernorm
        layernorm_type = LayerNormType.LayerNorm
        rescale_before_lm_head = False
        logits_dtype = str_dtype_to_trt(args.dtype)
        n_mels = config['n_mels']
    else:
        q_scaling = 1 / config['head_size']**.5
        has_attention_qkvo_bias = False
        has_mlp_bias = False
        has_model_final_layernorm = True
        has_position_embedding = False
        has_embedding_layernorm = False
        layernorm_type = LayerNormType.RmsNorm
        relative_attention = True
        layernorm_position = LayerNormPositionType.pre_layernorm
        if family == 't5':
            rescale_before_lm_head = True
        else:
            rescale_before_lm_head = False

    quant_config = get_quant_config(args.quantization)
    quant_mode = quant_config.quant_mode
    use_weight_only = quant_mode.is_weight_only()
    is_weight_streaming = getattr(args, "weight_streaming", False)

    builder = Builder()
    builder_config = builder.create_builder_config(
        name=args.model,
        precision=args.dtype,
        timing_cache=args.input_timing_cache,
        profiling_verbosity=args.profiling_verbosity,
        tensor_parallel=world_size,  # TP only
        parallel_build=True,
        num_layers=config['num_layers'],
        num_heads=config['num_heads'],
        hidden_size=config['hidden_size'],
        head_size=config['head_size'],
        vocab_size=config['vocab_size'],
        hidden_act=config['hidden_act'],
        max_position_embeddings=config['n_positions'],
        apply_query_key_layer_scaling=False,  # by default, hardcoded
        max_batch_size=config['max_batch_size'],
        max_beam_width=config['max_beam_width'],
        max_decoder_input_len=config['max_decoder_input_len'],
        max_output_len=config['max_output_len'],
        max_encoder_input_len=config['max_encoder_input_len'],
        opt_level=config['builder_opt'],
        cross_attention=(component == 'decoder'),
        has_position_embedding=has_position_embedding,
        has_token_type_embedding=False,  # by default
        strongly_typed=False,  # by default
        gather_all_token_logits=False,  # by default
        int8=(quant_mode.has_act_and_weight_quant()
              or quant_mode.is_int8_weight_only()),
        quant_mode=quant_mode,
        n_mels=n_mels,
        skip_cross_qkv=config['skip_cross_qkv'],
        weight_streaming=is_weight_streaming,
    )

    # build engine
    dtype = str_dtype_to_trt(args.dtype)

    mapping = Mapping(world_size=world_size,
                      rank=runtime_rank,
                      tp_size=world_size,
                      pp_size=1)  # TP only

    if component == 'encoder':
        if family == 'whisper':
            tllm_model = tensorrt_llm.models.WhisperEncoder(
                n_mels=config['n_mels'],
                n_ctx=1500,  # n_audio_ctx
                n_state=config['hidden_size'],
                n_head=config['num_heads'],
                n_layer=config['num_layers'],
                dtype=dtype)
            if use_weight_only:
                tllm_model = quantize(tllm_model, quant_config)
        else:
            pretrained_config = PretrainedConfig.from_dict({
                'architecture':
                "EncoderModel",
                'dtype':
                args.dtype,
                'logits_dtype':
                logits_dtype,
                'num_hidden_layers':
                config['num_layers'],
                'num_attention_heads':
                config['num_heads'],
                'hidden_size':
                config['hidden_size'],
                'norm_epsilon':
                1e-6,
                'vocab_size':
                config['vocab_size'],
                'hidden_act':
                config['hidden_act'],
                'mapping': {
                    'world_size': mapping.world_size,
                    'tp_size': mapping.tp_size,
                    'pp_size': mapping.pp_size,
                },
                'use_parallel_embedding':
                False,
                'embedding_sharding_dim':
                0,
                'max_position_embeddings':
                config.get('n_positions', 0),
                'use_prompt_tuning':
                False,
                'head_size':
                config['head_size'],
                'has_position_embedding':
                has_position_embedding,
                'layernorm_type':
                layernorm_type,
                'has_attention_qkvo_bias':
                has_attention_qkvo_bias,
                'has_mlp_bias':
                has_mlp_bias,
                'has_model_final_layernorm':
                has_model_final_layernorm,
                'has_embedding_layernorm':
                has_embedding_layernorm,
                'has_embedding_scale':
                config.get('has_embedding_scale', False),
                'ffn_hidden_size':
                config['ffn_hidden_size'],
                'q_scaling':
                q_scaling,
                'layernorm_position':
                layernorm_position,
                'relative_attention':
                relative_attention,
                'max_distance':
                config.get('max_distance', 0),
                'num_buckets':
                config.get('num_buckets', 0),
                'model_type':
                family,
            })
            tllm_model = tensorrt_llm.models.EncoderModel(pretrained_config)
    elif component == 'decoder':
        pretrained_config = PretrainedConfig.from_dict({
            'architecture':
            "DecoderModel",
            'dtype':
            args.dtype,
            'logits_dtype':
            logits_dtype,
            'num_hidden_layers':
            config['num_layers'],
            'num_attention_heads':
            config['num_heads'],
            'hidden_size':
            config['hidden_size'],
            'norm_epsilon':
            1e-6,
            'vocab_size':
            config['vocab_size'],
            'hidden_act':
            config['hidden_act'],
            'mapping': {
                'world_size': mapping.world_size,
                'tp_size': mapping.tp_size,
                'pp_size': mapping.pp_size,
            },
            'use_parallel_embedding':
            False,
            'embedding_sharding_dim':
            0,
            'max_position_embeddings':
            config.get('n_positions', 0),
            'use_prompt_tuning':
            False,
            'head_size':
            config['head_size'],
            'has_position_embedding':
            has_position_embedding,
            'layernorm_type':
            layernorm_type,
            'has_attention_qkvo_bias':
            has_attention_qkvo_bias,
            'has_mlp_bias':
            has_mlp_bias,
            'has_model_final_layernorm':
            has_model_final_layernorm,
            'has_embedding_layernorm':
            has_embedding_layernorm,
            'has_embedding_scale':
            config.get('has_embedding_scale', False),
            'ffn_hidden_size':
            config['ffn_hidden_size'],
            'q_scaling':
            q_scaling,
            'layernorm_position':
            layernorm_position,
            'relative_attention':
            relative_attention,
            'max_distance':
            config.get('max_distance', 0),
            'num_buckets':
            config.get('num_buckets', 0),
            'model_type':
            family,
            'rescale_before_lm_head':
            rescale_before_lm_head,
            'encoder_hidden_size':
            config['hidden_size'],
            'encoder_num_heads':
            config['num_heads'],
            'encoder_head_size':
            config['head_size'],
            'skip_cross_qkv':
            config['skip_cross_qkv']
        })
        tllm_model = tensorrt_llm.models.DecoderModel(pretrained_config)
        if use_weight_only and family == 'whisper':
            tllm_model = quantize(tllm_model, quant_config)

    # Module -> Network
    engine_name = get_engine_name(args.model, args.dtype, world_size,
                                  runtime_rank)
    network = builder.create_network()
    network.trt_network.name = engine_name
    network.plugin_config.to_legacy_setting()

    # Plugins
    if args.mode == 'plugin':
        network.plugin_config.set_bert_attention_plugin(dtype=args.dtype)
        network.plugin_config.set_gemm_plugin(dtype=args.dtype)
        network.plugin_config.set_gpt_attention_plugin(dtype=args.dtype)
        if use_weight_only:
            network.plugin_config.set_weight_only_quant_matmul_plugin(
                dtype=args.dtype)
    elif args.mode == 'ootb-except-mha':
        network.plugin_config.set_bert_attention_plugin(dtype=args.dtype)
        network.plugin_config.set_gpt_attention_plugin(dtype=args.dtype)

    if world_size > 1:
        network.plugin_config.set_nccl_plugin(
            dtype=args.dtype, use_custom_all_reduce=False)  # by default

    with net_guard(network):
        # Prepare
        network.set_named_parameters(tllm_model.named_parameters())

        # Forward
        if component == 'encoder':
            if family == 'whisper':
                inputs = tllm_model.prepare_inputs(
                    max_batch_size=config['max_batch_size'], )
                tllm_model(*inputs)
            else:
                inputs = tllm_model.prepare_inputs(
                    max_batch_size=config['max_batch_size'],
                    max_input_len=config['max_encoder_input_len'],
                )
                tllm_model(**inputs)
        elif component == 'decoder':
            if family == 'whisper':
                inputs = tllm_model.prepare_inputs(
                    max_batch_size=config['max_batch_size'],
                    max_beam_width=config['max_beam_width'],
                    max_decoder_input_len=config['max_decoder_input_len'],
                    max_seq_len=config['max_output_len'],
                    max_encoder_input_len=1500,  # n_audio_ctx
                )
                tllm_model(**inputs)
            else:
                inputs = tllm_model.prepare_inputs(
                    max_batch_size=config['max_batch_size'],
                    max_beam_width=config['max_beam_width'],
                    max_decoder_input_len=config['max_decoder_input_len'],
                    max_seq_len=config['max_output_len'],
                    max_encoder_input_len=config['max_encoder_input_len'],
                )

                tllm_model(**inputs)

    start = time.time()
    engine = builder.build_engine(network, builder_config)
    assert engine is not None, f'Failed to build engine for rank {runtime_rank}'
    build_time = round(time.time() - start, 2)

    # Get model config
    num_heads = config['num_heads']
    assert (num_heads % world_size) == 0
    num_heads = num_heads // world_size
    hidden_size = config['hidden_size'] // world_size
    model_config = tensorrt_llm.runtime.ModelConfig(
        num_heads=num_heads,
        num_kv_heads=num_heads,
        hidden_size=hidden_size,
        head_size=builder_config.head_size,
        max_batch_size=builder_config.max_batch_size,
        max_beam_width=builder_config.max_beam_width,
        vocab_size=builder_config.vocab_size,
        num_layers=builder_config.num_layers,
        gpt_attention_plugin=network.plugin_config.gpt_attention_plugin,
        remove_input_padding=network.plugin_config.remove_input_padding,
        cross_attention=builder_config.cross_attention,
        has_position_embedding=builder_config.has_position_embedding,
        has_token_type_embedding=builder_config.has_token_type_embedding,
        use_custom_all_reduce=False,  # by default
        dtype=dtype,
    )

    if args.output_dir is not None:
        output_dir = os.path.join(args.output_dir, component)
        if not os.path.exists(output_dir):
            os.makedirs(output_dir)
        serialize_path = os.path.join(output_dir, engine_name)
        serialize_engine(engine, serialize_path)
        if runtime_rank == 0:
            config_path = os.path.join(output_dir, 'config.json')
            builder_config.plugin_config = network.plugin_config
            builder.save_config(builder_config, config_path)
            if args.output_timing_cache:
                # Save timing cache to output_dir if not absolute path
                timing_cache_path = args.output_timing_cache if os.path.isabs(
                    args.output_timing_cache) else os.path.join(
                        args.output_dir, args.output_timing_cache)
                ok = builder.save_timing_cache(builder_config,
                                               timing_cache_path)
                if not ok:
                    logger.warning("Failed to save timing cache.")
    return engine, model_config, build_time


def build_enc_dec(args):
    build_config = get_build_config(args.model)
    if args.force_num_layer_1:
        build_config['num_layers'] = 1

    build_config['max_batch_size'] = build_config['max_batch_size'] \
        if args.max_batch_size is None else args.max_batch_size
    build_config['max_encoder_input_len'] = build_config['max_encoder_input_len'] \
        if args.max_input_len is None else args.max_input_len
    build_config['max_decoder_input_len'] = 1
    build_config['max_output_len'] = build_config['max_output_len'] \
        if args.max_output_len is None else args.max_output_len
    build_config[
        'max_beam_width'] = 1 if args.max_beam_width is None else args.max_beam_width

    encoder_engine, encoder_model_config, encoder_build_time = enc_dec_build_helper(
        component='encoder', config=build_config, args=args)
    decoder_engine, decoder_model_config, decoder_build_time = enc_dec_build_helper(
        component='decoder', config=build_config, args=args)

    return encoder_engine, decoder_engine, encoder_model_config, decoder_model_config, encoder_build_time, decoder_build_time


def main(args):
    logger.set_level(args.log_level)
    if args.model in get_allowed_models(benchmark_type="gpt"):
        engine = build_gpt(args)[0]
        engine_size = engine.nbytes
    elif args.model in get_allowed_models(benchmark_type="bert"):
        engine = build_bert(args)[0]
        engine_size = engine.nbytes
    elif args.model in get_allowed_models(benchmark_type="enc_dec"):
        encoder_engine, decoder_engine = build_enc_dec(args)[:2]
        engine_size = encoder_engine.nbytes + decoder_engine.nbytes
    else:
        raise Exception(f'Unexpected model: {args.model}')

    # Print engine size for CI/CD to track.
    logger.info(
        f"Total engine size per GPU is {engine_size / 1048576:.2f} MiB.")


if __name__ == '__main__':
    mp.set_start_method('spawn')
    args = parse_arguments()
    main(args)
