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
import math
import unittest

import numpy as np

# isort: off
import torch
import tensorrt as trt
# isort: on
import os
import sys

from parameterized import parameterized
from polygraphy.backend.trt import (CreateConfig, EngineFromNetwork, Profile,
                                    TrtRunner)

import tensorrt_llm
from tensorrt_llm import Tensor
from tensorrt_llm._utils import torch_to_numpy, trt_dtype_to_torch
from tensorrt_llm.layers.moe import MoeConfig
from tensorrt_llm.quantization import QuantMode

sys.path.append(os.path.join(os.path.dirname(__file__), '..'))
from utils.util import getSMVersion, skip_bf16_pre_ampere, unittest_name_func

default_actfn = 'gelu'
default_hidden_size = {
    'float32': 8,
    'float16': 8,
    'bfloat16': 8,
    'int8': 64,
    'int4': 64,
    'fp8': 16,
}


def make_tuple(num_experts=4,
               topk=1,
               hidden_size=None,
               actfn=default_actfn,
               bias=True,
               dtype='float16',
               weight_dtype=None,
               norm_mode=MoeConfig.ExpertScaleNormalizationMode.NONE,
               use_plugin=True):
    if weight_dtype is None:
        weight_dtype = dtype
    if hidden_size is None:
        hidden_size = default_hidden_size[weight_dtype]
    return (num_experts, topk, hidden_size, actfn, bias, dtype, weight_dtype,
            norm_mode, use_plugin)


def config_is_allowed(config):
    # TODO: Support ootb path with getSMVersion() < 90:
    enable_ootb = getSMVersion() >= 90
    enable_bf16 = getSMVersion() >= 80
    enable_fp8 = getSMVersion() >= 90

    DATA_TYPE_INDEX = 5
    WEIGHT_TYPE_INDEX = 6
    USE_PLUGIN_INDEX = 8
    if not enable_fp8 and config[WEIGHT_TYPE_INDEX] == 'fp8':
        return False
    if not enable_bf16 and config[DATA_TYPE_INDEX] == 'bfloat16':
        return False
    if not enable_ootb and not config[USE_PLUGIN_INDEX]:
        return False
    return True


def gen_uniform_weights(*args, **kwargs):
    return (torch.rand(*args, **kwargs) * 2 - 1).contiguous()


def quant_dequant_int(weights, quant_mode):
    # use the test version `_symmetric_...` to get the non-interleaved weights
    type = torch.quint4x2 if quant_mode.is_int4_weight_only() else torch.int8
    quant_weights, _, torch_weight_scales = torch.ops.trtllm._symmetric_quantize_last_axis_of_batched_matrix(
        weights.T.cpu().contiguous(), type)

    # Unpack the int4s int int8s
    if quant_mode.is_int4_weight_only():
        upper = (quant_weights >> 4)
        lower = (quant_weights << 4) >> 4  # Arithmetic right shift sign extends
        quant_weights = torch.stack((lower, upper), dim=2).view(weights.T.shape)

    quant_weights = quant_weights.to(dtype=weights.dtype)
    result = torch.multiply(quant_weights,
                            torch_weight_scales.unsqueeze(0)).T.contiguous()
    return result.to(device=weights.device)


def quant_dequant(weights, quant_mode):
    if quant_mode.is_weight_only():
        return quant_dequant_int(weights, quant_mode)

    return weights


GATED_TO_ACT = {
    'swiglu': 'silu',
    'geglu': 'gelu',
}


def is_gated_activation(actfn):
    return actfn in GATED_TO_ACT


def gated2act(actfn):
    if is_gated_activation(actfn):
        return GATED_TO_ACT[actfn]
    return actfn


def doact(input, actfn):
    assert not is_gated_activation(actfn)
    if actfn == 'gelu':
        return torch.nn.functional.gelu(input)
    if actfn == 'relu':
        return torch.nn.functional.relu(input)
    if actfn == 'silu':
        return torch.nn.functional.silu(input)
    assert actfn == "identity"
    return input  # Identity


def gated_matmul(input, weights, bias, actfn):
    assert is_gated_activation(actfn)
    fc1 = torch.matmul(input, weights.T) + bias
    fc1, gate = fc1.chunk(2, dim=-1)
    return fc1 * doact(gate, gated2act(actfn))


class TestFunctional(unittest.TestCase):

    def setUp(self):
        # There is a known precision issues where the topk may select different experts when the routing probabilities are similar.
        #  This causes a completely different output for the affected tokens. So we set the seed to prevent sporadic failures
        #  This shouldn't be a problem for most practical applications as it means the experts are equally good choices
        torch.manual_seed(0x766E)
        tensorrt_llm.logger.set_level('error')

    def eye(self, shape, dtype, device='cuda'):
        """ Utility function for creating expert weights as an identity matrix for easy debugging """
        eye = torch.eye(shape[-2], m=shape[-1], dtype=dtype, device=device)
        eye = eye.repeat(*shape[:-2], 1, 1)
        return eye

    @staticmethod
    def get_params():
        params = []
        params += [
            make_tuple(num_experts=1, topk=1, dtype='float16'),
            make_tuple(num_experts=4, topk=2, dtype='float16'),
            # Non-powers of two have special handling for plugin softmax
            make_tuple(num_experts=42, topk=3, dtype='float16'),
            # Experts > 256 have special handling for plugin softmax
            make_tuple(num_experts=1024, topk=3, dtype='float16'),
        ]
        # OOTB test
        params += [
            make_tuple(num_experts=1, topk=1, dtype='float16',
                       use_plugin=False),
            make_tuple(num_experts=4, topk=2, dtype='float16',
                       use_plugin=False),
            make_tuple(num_experts=42,
                       topk=3,
                       dtype='float16',
                       use_plugin=False),
        ]

        # Hidden size
        params += [
            make_tuple(hidden_size=128, dtype='float16'),
        ]

        # Add a test for float32
        params += [
            make_tuple(dtype='float32'),
            make_tuple(dtype='float32', use_plugin=False),
        ]

        # Add a test for bfloat16
        params += [
            make_tuple(dtype='bfloat16'),
        ]

        # Add some cases for quantized dtype
        for dtype in ('int8', 'int4'):
            params += [
                make_tuple(dtype='float16', hidden_size=64, weight_dtype=dtype),
            ]
            params += [
                make_tuple(dtype='bfloat16', hidden_size=64, weight_dtype=dtype)
            ]

        # fp8 tests
        params += [
            make_tuple(weight_dtype='fp8', bias=False),
            make_tuple(dtype='bfloat16', weight_dtype='fp8', bias=False),
            make_tuple(topk=2, weight_dtype='fp8', bias=False),
            make_tuple(num_experts=5, topk=2, weight_dtype='fp8', bias=False),
        ]

        # Test all activation functions with float16
        for actfn in ('relu', 'silu', 'gelu', 'swiglu', 'geglu', 'identity'):
            if actfn == default_actfn:
                continue  # Dont need to retest the activation function every other case uses

            params += [
                make_tuple(actfn=actfn, dtype='float16'),
                make_tuple(actfn=actfn, dtype='float16', use_plugin=False)
            ]

        # Test gated with all data types as it has a different path
        for actfn in ('swiglu', 'geglu'):
            if actfn == default_actfn:
                continue  # Dont need to retest the one every other case uses
            params += [
                make_tuple(actfn=actfn, dtype='float32'),
                make_tuple(actfn=actfn, dtype='float16', weight_dtype='int8'),
                make_tuple(actfn=actfn, dtype='bfloat16'),
                make_tuple(actfn='geglu',
                           dtype='float16',
                           weight_dtype='fp8',
                           bias=False)
            ]

        # Test different k values for gated activations (regression case)
        params += [
            make_tuple(actfn='geglu', topk=2, dtype='float16'),
        ]

        # Test no bias
        params += [
            make_tuple(bias=False, dtype='float32'),
            make_tuple(bias=False, dtype='float16'),
            make_tuple(dtype='float16', weight_dtype='int8', bias=False),
            make_tuple(dtype='float16', weight_dtype='int4', bias=False),
        ]

        # Test renormalization
        params += [
            make_tuple(
                topk=2,
                dtype='float32',
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE),
            make_tuple(
                topk=2,
                dtype='float16',
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE),
            make_tuple(
                dtype='bfloat16',
                topk=2,
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE),
            make_tuple(
                weight_dtype='fp8',
                topk=2,
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE,
                bias=False),
            # Renorm affects the final accumulate, so sanity check with no bias too
            make_tuple(
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE,
                topk=2,
                dtype='float16',
                bias=False),
        ]

        # Test OOTB renormalization
        params += [
            make_tuple(
                topk=2,
                dtype='float32',
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE,
                use_plugin=False),
            make_tuple(
                topk=2,
                dtype='float16',
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE,
                use_plugin=False),
            make_tuple(
                topk=2,
                dtype='bfloat16',
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE,
                use_plugin=False),
        ]

        # Default configuration for mixtral
        params += [
            make_tuple(
                num_experts=8,
                topk=2,
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE,
                hidden_size=4096,
                dtype='bfloat16',
                actfn='swiglu')
        ]

        filtered_params = []
        for p in params:
            if config_is_allowed(p):
                filtered_params.append(p)

        return filtered_params

    def create_weights(self, num_experts, hidden_size, ffn_hidden_size, bias,
                       dtype, weight_dtype, is_gated):
        self.router_weights = torch.randn((num_experts, hidden_size),
                                          dtype=torch.float32,
                                          device="cuda")
        # Use a uniform scale for int8 so the quantization has a well-behaved dynamic range
        genfn = gen_uniform_weights if weight_dtype == trt.int8 else torch.randn

        # Rescale the weights if we are using gated so the results are in a similar range
        # This is 'about right' to keep the variance the same based on some napkin maths
        fc1_weight_rescale = 1 / math.sqrt(2) if is_gated else 1
        fc2_weight_rescale = 1
        if genfn == torch.randn:
            fc1_weight_rescale *= math.sqrt(2.0 / ffn_hidden_size)
            fc2_weight_rescale *= math.sqrt(2.0 / hidden_size)

        fc1_out_size = ffn_hidden_size * 2 if is_gated else ffn_hidden_size
        self.fc1_weights = genfn((num_experts, fc1_out_size, hidden_size),
                                 dtype=trt_dtype_to_torch(dtype),
                                 device="cuda") * fc1_weight_rescale

        self.fc2_weights = genfn((num_experts, hidden_size, ffn_hidden_size),
                                 dtype=trt_dtype_to_torch(dtype),
                                 device="cuda") * fc2_weight_rescale

        bias_tensor_func = genfn if bias else torch.zeros
        self.fc1_bias = bias_tensor_func((num_experts, fc1_out_size),
                                         dtype=trt_dtype_to_torch(dtype),
                                         device="cuda")

        self.fc2_bias = bias_tensor_func((num_experts, hidden_size),
                                         dtype=trt_dtype_to_torch(dtype),
                                         device="cuda")

        # Set later
        self.weight_scaling_factor_1 = None
        self.weight_scaling_factor_2 = None
        self.activation_scaling_factor_1 = None
        self.activation_scaling_factor_2 = None

    def create_fp8_scaling_factors(self, max_act1, max_act2):
        self.activation_scaling_factor_1 = torch.tensor([max_act1
                                                         ]).float() / 440.
        self.activation_scaling_factor_2 = torch.tensor([max_act2
                                                         ]).float() / 440.

        def max_weights(weights):
            return torch.max(torch.abs(weights.view(weights.shape[0], -1)),
                             dim=1,
                             keepdim=True)[0].float()

        self.weight_scaling_factor_1 = max_weights(self.fc1_weights) / 440.
        self.weight_scaling_factor_2 = max_weights(self.fc2_weights) / 440.

    @parameterized.expand(get_params(), name_func=unittest_name_func)
    def test_mixture_of_experts(self, num_experts, top_k, hidden_size, actfn,
                                bias, dtype_str, weight_dtype_str, norm_mode,
                                use_plugin):
        """ This test compares the MOE result to a simple reference implementation using torch """

        # Build time is also proportional to the size of these (more plugin profiler runs) so dont make them too big
        # TODO Increasing these also cause some failures (observed on Hopper), not sure if this is a problem or not
        max_num_seq = 10
        max_seq_len = 4

        dtype = tensorrt_llm.str_dtype_to_trt(dtype_str)

        use_fp8_qdq = weight_dtype_str == 'fp8'
        use_int4_weights = weight_dtype_str == 'int4'
        weight_dtype = trt.int8 if use_int4_weights else tensorrt_llm.str_dtype_to_trt(
            weight_dtype_str)

        quant_mode = QuantMode(0)
        if use_fp8_qdq:
            quant_mode = quant_mode.set_fp8_qdq()
        elif weight_dtype != dtype:
            quant_mode = QuantMode.use_weight_only(
                use_int4_weights=use_int4_weights)

        ffn_hidden_size = 4 * hidden_size
        self.create_weights(num_experts,
                            hidden_size,
                            ffn_hidden_size,
                            bias,
                            dtype,
                            weight_dtype,
                            is_gated=is_gated_activation(actfn))

        sequence_sizes = [(1, 1), (max_num_seq, max_seq_len)]
        inputs = [gen_uniform_weights((num_seq, seq_len, hidden_size), dtype=trt_dtype_to_torch(dtype)) \
                  for num_seq, seq_len in sequence_sizes]
        reference_values = []

        act_1_quant = max(*[torch.max(torch.abs(v)).item() for v in inputs])
        act_2_quant = 0.0

        for i, input in enumerate(inputs):
            result, act2_quant_values = self.referenceImpl(
                input, top_k, actfn, weight_dtype, quant_mode, norm_mode)
            reference_values.append(result.cpu().float())
            act_2_quant = max(act_2_quant, act2_quant_values)

        self.create_fp8_scaling_factors(act_1_quant, act_2_quant)

        engine = self.buildTrtEngine(
            (-1, -1, hidden_size),
            num_experts,
            top_k,
            hidden_size,
            ffn_hidden_size,
            actfn,
            bias,
            dtype,
            weight_dtype=weight_dtype,
            quant_mode=quant_mode,
            norm_mode=norm_mode,
            use_plugin=use_plugin,
            max_sizes=[max_num_seq, max_seq_len, hidden_size])

        for input, ref in zip(inputs, reference_values):
            # construct trt network
            trt_res = self.runTrtEngine(engine, input)['output'].float()

            tolerances = {
                'float32': 1e-2,
                'float16': 5e-2,
                'bfloat16': 5e-2,
                'int8': 2e-1,
                'int4': 2e-1,
                'fp8': 2e-1,
            }
            tolerance = tolerances[weight_dtype_str]

            # Bit of a hack to allow bigger tolerance for the Mixtral tests
            if hidden_size > 1024:
                # Do some extra checks on the full distribution
                self.assertAlmostEqual(np.mean((trt_res - ref).numpy()),
                                       0.0,
                                       delta=2e-4)
                self.assertAlmostEqual(np.var((trt_res - ref).numpy()),
                                       0.0,
                                       delta=tolerance)
                # Set a higher tolerance because we hit a small fraction of outlier cases (<<1%)
                tolerance = 0.3

            np.testing.assert_allclose(trt_res,
                                       ref,
                                       rtol=tolerance,
                                       atol=tolerance)

    @staticmethod
    def get_mlp_params():
        params = []
        for actfn in ('gelu', 'geglu'):
            params += [('float32', actfn, True), ('float16', actfn, True),
                       ('bfloat16', actfn, True), ('int8', actfn, True),
                       ('int4', actfn, True)]
            # OOTB tests
            # TODO: Support ootb path with getSMVersion() < 90, quantization:
            if getSMVersion() >= 90:
                params += [('float32', actfn, False), ('float16', actfn, False),
                           ('bfloat16', actfn, False)]
        return params

    @parameterized.expand(get_mlp_params(), name_func=unittest_name_func)
    def test_mlp_comparison(self, dtype_str, actfn, use_plugin):
        """ This test uses one expert and compares the result to a plain MLP """
        skip_bf16_pre_ampere(dtype_str)

        use_int4_weights = dtype_str == 'int4'
        weight_dtype = trt.int8 if use_int4_weights else tensorrt_llm.str_dtype_to_trt(
            dtype_str)

        dtype = weight_dtype
        quant_mode = QuantMode(0)
        hidden_size = 8
        if dtype_str == 'int8' or dtype_str == 'int4':
            dtype = tensorrt_llm.str_dtype_to_trt("float16")
            hidden_size = 64
            quant_mode = QuantMode.use_weight_only(
                use_int4_weights=use_int4_weights)

        num_sequences = 5
        sequence_lengths = 4
        num_experts = 1
        top_k = 1
        bias = True
        ffn_hidden_size = 4 * hidden_size
        self.create_weights(num_experts,
                            hidden_size,
                            ffn_hidden_size,
                            bias,
                            dtype,
                            weight_dtype,
                            is_gated=is_gated_activation(actfn))

        input_data = gen_uniform_weights(
            (num_sequences, sequence_lengths, hidden_size),
            dtype=trt_dtype_to_torch(dtype))

        def MLP(network, trt_key):
            mlp_type = tensorrt_llm.layers.GatedMLP if is_gated_activation(
                actfn) else tensorrt_llm.layers.MLP
            mlp = mlp_type(hidden_size=hidden_size,
                           ffn_hidden_size=ffn_hidden_size,
                           hidden_act=gated2act(actfn),
                           bias=bias,
                           quant_mode=quant_mode,
                           dtype=dtype)
            # Quantize the weights manually so the results are comparable
            fc1_qd = quant_dequant(self.fc1_weights[0].cpu(), quant_mode)
            if is_gated_activation(actfn):
                # Note that the MLP uses the opposite convention to the GLU paper for naming,
                #  the gate is the matrix the activations are NOT applied to
                gate, fc1_qd = fc1_qd.chunk(2, dim=0)
                mlp.gate.weight.value = np.ascontiguousarray(
                    torch_to_numpy(gate))

            mlp.fc.weight.value = np.ascontiguousarray(torch_to_numpy(fc1_qd))
            fc2_qd = quant_dequant(self.fc2_weights[0].cpu(), quant_mode)
            mlp.proj.weight.value = np.ascontiguousarray(torch_to_numpy(fc2_qd))
            if bias:
                fc1_bias = self.fc1_bias[0].cpu()

                if is_gated_activation(actfn):
                    gate, fc1_bias = fc1_bias.chunk(2, dim=0)
                    mlp.gate.bias.value = np.ascontiguousarray(
                        torch_to_numpy(gate))

                mlp.fc.bias.value = np.ascontiguousarray(
                    torch_to_numpy(fc1_bias))
                mlp.proj.bias.value = np.ascontiguousarray(
                    torch_to_numpy(self.fc2_bias[0].cpu()))

            output = mlp(trt_key).trt_tensor
            output.name = 'mlp_output'
            network.mark_output(output)
            output.dtype = dtype

        res = self.trtImpl(input_data,
                           num_experts,
                           top_k,
                           hidden_size,
                           ffn_hidden_size,
                           actfn,
                           bias,
                           dtype,
                           weight_dtype=weight_dtype,
                           quant_mode=quant_mode,
                           custom_network=MLP,
                           use_plugin=use_plugin)

        tolerances = {
            'float32': 1e-2,
            'float16': 2e-2
            if getSMVersion() >= 75 else 1e-1,  # Some issues for geglu on volta
            'bfloat16': 1e-1,
            'int8': 2e-1,
            'int4': 2e-1,
        }
        np.testing.assert_allclose(res['output'].float(),
                                   res['mlp_output'].float(),
                                   rtol=tolerances[dtype_str],
                                   atol=tolerances[dtype_str])

    def set_weight_layer(self,
                         input_weights,
                         moe_weight_wrapper,
                         quant_mode,
                         fp8_scalar=None):
        if quant_mode.is_weight_only():
            torch_transpose = torch.transpose(input_weights, 1,
                                              2).contiguous().cpu()
            type = torch.quint4x2 if quant_mode.is_int4_weight_only(
            ) else torch.int8
            processed_torch_weights, torch_weight_scales = torch.ops.trtllm.symmetric_quantize_last_axis_of_batched_matrix(
                torch_transpose, type)
            # Change the shape to what moe expects without touching the underlying format
            moe_weight_wrapper.weight.value = np.ascontiguousarray(
                torch_to_numpy(processed_torch_weights))
            moe_weight_wrapper.per_channel_scale.value = np.ascontiguousarray(
                torch_to_numpy(torch_weight_scales))
        elif quant_mode.has_fp8_qdq():
            processed_torch_weights = (input_weights /
                                       fp8_scalar.unsqueeze(-1)).to(
                                           torch.float8_e4m3fn)
            moe_weight_wrapper.weight.value = np.ascontiguousarray(
                torch_to_numpy(processed_torch_weights))
            moe_weight_wrapper.weights_scaling_factor.value = np.ascontiguousarray(
                torch_to_numpy(fp8_scalar))
        else:
            moe_weight_wrapper.weight.value = np.ascontiguousarray(
                torch_to_numpy(input_weights))

    def buildTrtEngine(self,
                       input_shape,
                       num_experts,
                       top_k,
                       hidden_size,
                       ffn_hidden_size,
                       actfn,
                       bias,
                       dtype: trt.DataType,
                       weight_dtype: trt.DataType,
                       quant_mode,
                       norm_mode,
                       custom_network=None,
                       use_plugin=True,
                       max_sizes=None):
        builder = tensorrt_llm.Builder()
        builder.strongly_typed = weight_dtype == trt.fp8
        net = builder.create_network()
        net.plugin_config.set_moe_plugin(dtype if use_plugin else None)
        with tensorrt_llm.net_guard(net):
            network = tensorrt_llm.default_trtnet()
            trt_key = Tensor(name='input_hidden_states',
                             shape=input_shape,
                             dtype=dtype)

            moe = tensorrt_llm.layers.MOE(moe_config=MoeConfig(
                num_experts=num_experts,
                top_k=top_k,
                normalization_mode=norm_mode),
                                          hidden_size=hidden_size,
                                          ffn_hidden_size=ffn_hidden_size,
                                          hidden_act=actfn,
                                          bias=bias,
                                          dtype=dtype,
                                          quant_mode=quant_mode)
            moe.router.weight.value = torch_to_numpy(self.router_weights.cpu())

            self.set_weight_layer(self.fc1_weights, moe.fc, quant_mode,
                                  self.weight_scaling_factor_1)
            self.set_weight_layer(self.fc2_weights, moe.proj, quant_mode,
                                  self.weight_scaling_factor_2)

            if quant_mode.has_fp8_qdq():
                moe.fc.activation_scaling_factor.value = torch_to_numpy(
                    self.activation_scaling_factor_1)
                moe.proj.activation_scaling_factor.value = torch_to_numpy(
                    self.activation_scaling_factor_2)
                moe.fc.weights_scaling_factor.value = torch_to_numpy(
                    self.weight_scaling_factor_1)
                moe.proj.weights_scaling_factor.value = torch_to_numpy(
                    self.weight_scaling_factor_2)

            if bias:
                moe.fc.bias.value = torch_to_numpy(self.fc1_bias.cpu())
                moe.proj.bias.value = torch_to_numpy(self.fc2_bias.cpu())

            if custom_network:
                custom_network(network, trt_key)

            output = moe(trt_key).trt_tensor
            output.name = 'output'
            network.mark_output(output)
            output.dtype = dtype

        profiles = None
        if max_sizes:
            profiles = [
                Profile().add('input_hidden_states', (1, 1, hidden_size),
                              (1, 1, hidden_size), max_sizes)
            ]

        config = CreateConfig(builder_optimization_level=4, profiles=profiles)
        if not builder.strongly_typed:
            config = CreateConfig(fp16=(dtype == trt.float16),
                                  bf16=(dtype == trt.bfloat16),
                                  int8=(weight_dtype == trt.int8),
                                  fp8=(weight_dtype == trt.fp8),
                                  precision_constraints='obey',
                                  builder_optimization_level=4,
                                  profiles=profiles)

        # trt run
        build_engine = EngineFromNetwork((builder.trt_builder, net.trt_network),
                                         config=config)
        assert build_engine is not None
        return build_engine

    def runTrtEngine(self, engine, input_data):
        with TrtRunner(engine) as runner:
            feed_dict = {
                'input_hidden_states': input_data,
            }
            outputs = runner.infer(feed_dict=feed_dict)
        return outputs

    def trtImpl(self,
                input_data,
                num_experts,
                top_k,
                hidden_size,
                ffn_hidden_size,
                actfn,
                bias,
                dtype: trt.DataType,
                weight_dtype: trt.DataType = None,
                quant_mode=QuantMode(0),
                norm_mode=MoeConfig.ExpertScaleNormalizationMode.NONE,
                custom_network=None,
                use_plugin=True):
        build_engine = self.buildTrtEngine(tuple(input_data.shape), num_experts,
                                           top_k, hidden_size, ffn_hidden_size,
                                           actfn, bias, dtype, weight_dtype,
                                           quant_mode, norm_mode,
                                           custom_network, use_plugin)

        outputs = self.runTrtEngine(build_engine, input_data)
        return outputs

    def referenceImpl(self, inputs, k, actfn, weight_dtype, quant_mode,
                      norm_mode):
        # Always run the ref implementation at full precision TODO is this a good choice?
        inputs = inputs.cuda().float()
        inputs_merged = inputs.view(-1, inputs.shape[-1])
        routing = torch.matmul(inputs_merged, self.router_weights.T.float())
        assert routing.shape == (inputs_merged.shape[0],
                                 self.router_weights.shape[0])
        router_probs = torch.softmax(routing, 1, dtype=inputs.dtype)
        assert routing.shape == router_probs.shape

        topk = torch.topk(router_probs, k)
        assert topk.indices.shape == (router_probs.shape[0], k)
        max_act_2 = 0.0
        results = torch.zeros_like(inputs_merged)
        for i, (scales, experts) in enumerate(zip(topk.values, topk.indices)):
            if norm_mode == MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE:
                scales /= sum(scales)
            input = inputs_merged[i, :]
            for scale, expert in zip(scales, experts):
                fc1_qd = quant_dequant(self.fc1_weights[expert], quant_mode)
                if is_gated_activation(actfn):
                    fc1 = gated_matmul(input, fc1_qd.float(),
                                       self.fc1_bias[expert].float(), actfn)
                else:
                    fc1 = torch.matmul(
                        input,
                        fc1_qd.T.float()) + self.fc1_bias[expert].float()
                    fc1 = doact(fc1, actfn)

                max_act_2 = max(max_act_2, torch.max(torch.abs(fc1)).item())

                fc2_qd = quant_dequant(self.fc2_weights[expert], quant_mode)
                final = torch.matmul(
                    fc1, fc2_qd.T.float()) + self.fc2_bias[expert].float()
                assert final.shape == (inputs.shape[-1], )
                results[i] += scale * final
        return results.view(*inputs.shape), max_act_2


if __name__ == "__main__":
    unittest.main()
