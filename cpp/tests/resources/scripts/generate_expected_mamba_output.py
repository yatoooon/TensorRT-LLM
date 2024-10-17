#!/usr/bin/env python3
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

from pathlib import Path

import run


def generate_output(engine: str,
                    num_beams: int,
                    input_name: str,
                    output_name: str,
                    max_output_len: int = 8,
                    output_logits: bool = False):
    tp_size = 1
    pp_size = 1
    model = 'mamba-2.8b-hf'
    resources_dir = Path(__file__).parent.resolve().parent
    models_dir = resources_dir / 'models'
    tp_pp_dir = 'tp' + str(tp_size) + '-pp' + str(pp_size) + '-gpu/'
    engine_dir = models_dir / 'rt_engine' / model / engine / tp_pp_dir

    data_dir = resources_dir / 'data'
    input_file = data_dir / (input_name + '.npy')
    model_data_dir = data_dir / model
    if num_beams <= 1:
        output_dir = model_data_dir / 'sampling'
    else:
        output_dir = model_data_dir / ('beam_search_' + str(num_beams))

    output_name += '_tp' + str(tp_size) + '_pp' + str(pp_size)

    output_logits_npy = None
    if output_logits:
        output_logits_npy = str(output_dir / (output_name + '_logits' + '.npy'))

    args = run.parse_arguments([
        '--engine_dir',
        str(engine_dir), '--input_file',
        str(input_file), '--tokenizer_dir',
        str(models_dir / 'gpt-neox-20b'), '--output_npy',
        str(output_dir / (output_name + '.npy')), '--output_csv',
        str(output_dir / (output_name + '.csv')), '--max_output_len',
        str(max_output_len), '--num_beams',
        str(num_beams), '--output_logits_npy',
        str(output_logits_npy), '--use_py_session'
    ])
    run.main(args)


def generate_outputs(num_beams):
    print('Generating Mamba FP16 outputs')
    generate_output(engine='fp16-default',
                    num_beams=num_beams,
                    input_name='input_tokens',
                    output_name='output_tokens_fp16')
    print('Generating Mamba FP16-plugin outputs')
    generate_output(engine='fp16-plugin',
                    num_beams=num_beams,
                    input_name='input_tokens',
                    output_name='output_tokens_fp16_plugin')
    print('Generating Mamba FP16-plugin-packed outputs')
    generate_output(engine='fp16-plugin-packed',
                    num_beams=num_beams,
                    input_name='input_tokens',
                    output_name='output_tokens_fp16_plugin_packed')
    print('Generating Mamba FP16-plugin-packed-paged outputs')
    generate_output(engine='fp16-plugin-packed-paged',
                    num_beams=num_beams,
                    input_name='input_tokens',
                    output_name='output_tokens_fp16_plugin_packed_paged')


if __name__ == '__main__':
    generate_outputs(num_beams=1)
