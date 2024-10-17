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
from .baichuan.model import BaichuanForCausalLM
from .bert.model import (BertForQuestionAnswering,
                         BertForSequenceClassification, BertModel)
from .bloom.model import BloomForCausalLM, BloomModel
from .chatglm.model import ChatGLMForCausalLM, ChatGLMModel
from .cogvlm.model import CogVLMForCausalLM
from .dbrx.model import DbrxForCausalLM
from .enc_dec.model import DecoderModel, EncoderModel, WhisperEncoder
from .falcon.model import FalconForCausalLM, FalconModel
from .gemma.model import GemmaForCausalLM
from .gpt.model import GPTForCausalLM, GPTModel
from .gptj.model import GPTJForCausalLM, GPTJModel
from .gptneox.model import GPTNeoXForCausalLM, GPTNeoXModel
from .llama.model import LLaMAForCausalLM, LLaMAModel
from .mamba.model import MambaForCausalLM
from .medusa.model import MedusaForCausalLm
from .modeling_utils import (PretrainedConfig, PretrainedModel,
                             SpeculativeDecodingMode)
from .mpt.model import MPTForCausalLM, MPTModel
from .opt.model import OPTForCausalLM, OPTModel
from .phi3.model import Phi3ForCausalLM, Phi3Model
from .phi.model import PhiForCausalLM, PhiModel
from .qwen.model import QWenForCausalLM
from .recurrentgemma.model import RecurrentGemmaForCausalLM

__all__ = [
    'BertModel',
    'BertForQuestionAnswering',
    'BertForSequenceClassification',
    'BloomModel',
    'BloomForCausalLM',
    'FalconForCausalLM',
    'FalconModel',
    'GPTModel',
    'GPTForCausalLM',
    'OPTForCausalLM',
    'OPTModel',
    'LLaMAForCausalLM',
    'LLaMAModel',
    'MedusaForCausalLm',
    'GPTJModel',
    'GPTJForCausalLM',
    'GPTNeoXModel',
    'GPTNeoXForCausalLM',
    'PhiModel',
    'Phi3Model',
    'PhiForCausalLM',
    'Phi3ForCausalLM',
    'ChatGLMForCausalLM',
    'ChatGLMModel',
    'BaichuanForCausalLM',
    'QWenForCausalLM',
    'EncoderModel',
    'DecoderModel',
    'PretrainedConfig',
    'PretrainedModel',
    'WhisperEncoder',
    'MambaForCausalLM',
    'MPTForCausalLM',
    'MPTModel',
    'SkyworkForCausalLM',
    'GemmaForCausalLM',
    'DbrxForCausalLM',
    'RecurrentGemmaForCausalLM',
    'CogVLMForCausalLM',
    'SpeculativeDecodingMode',
]

MODEL_MAP = {
    'GPTForCausalLM': GPTForCausalLM,
    'OPTForCausalLM': OPTForCausalLM,
    'BloomForCausalLM': BloomForCausalLM,
    'FalconForCausalLM': FalconForCausalLM,
    'PhiForCausalLM': PhiForCausalLM,
    'Phi3ForCausalLM': Phi3ForCausalLM,
    'MambaForCausalLM': MambaForCausalLM,
    'GPTNeoXForCausalLM': GPTNeoXForCausalLM,
    'GPTJForCausalLM': GPTJForCausalLM,
    'MPTForCausalLM': MPTForCausalLM,
    'ChatGLMForCausalLM': ChatGLMForCausalLM,
    'LlamaForCausalLM': LLaMAForCausalLM,
    'MistralForCausalLM': LLaMAForCausalLM,
    'MixtralForCausalLM': LLaMAForCausalLM,
    'ArcticForCausalLM': LLaMAForCausalLM,
    'InternLMForCausalLM': LLaMAForCausalLM,
    'MedusaForCausalLM': MedusaForCausalLm,
    'BaichuanForCausalLM': BaichuanForCausalLM,
    'SkyworkForCausalLM': LLaMAForCausalLM,
    'GemmaForCausalLM': GemmaForCausalLM,
    'QWenForCausalLM': QWenForCausalLM,
    'EncoderModel': EncoderModel,
    'DecoderModel': DecoderModel,
    'DbrxForCausalLM': DbrxForCausalLM,
    'RecurrentGemmaForCausalLM': RecurrentGemmaForCausalLM,
    'CogVLMForCausalLM': CogVLMForCausalLM,
}
