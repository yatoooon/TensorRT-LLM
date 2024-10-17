/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "tensorrt_llm/layers/banWordsLayer.h"
#include "tensorrt_llm/layers/baseLayer.h"
#include "tensorrt_llm/layers/decodingLayer.h"
#include "tensorrt_llm/layers/penaltyLayer.h"
#include "tensorrt_llm/layers/stopCriteriaLayer.h"
#include <memory>
#include <vector>

namespace tensorrt_llm::layers
{
enum DecodingLayers_t
{
    PENALTY_LAYER,
    BAN_WORDS_LAYER,
    DECODING_LAYER,
    STOP_CRITERIA_LAYER
};

static std::vector<DecodingLayers_t> createDecodingLayerTypes(runtime::DecodingMode const& mode)
{
    std::vector<DecodingLayers_t> types;
    if (mode.isTopKorTopP() || mode.isBeamSearch())
    {
        return {DecodingLayers_t::PENALTY_LAYER, DecodingLayers_t::BAN_WORDS_LAYER, DecodingLayers_t::DECODING_LAYER,
            DecodingLayers_t::STOP_CRITERIA_LAYER};
    }
    else if (mode.isMedusa())
    {
        return {
            DecodingLayers_t::PENALTY_LAYER, DecodingLayers_t::DECODING_LAYER, DecodingLayers_t::STOP_CRITERIA_LAYER};
    }
    TLLM_CHECK_WITH_INFO(false, "layer types are not defined for mode (%d)",
        *reinterpret_cast<runtime::DecodingMode::UnderlyingType const*>(&mode));
    return {};
}

template <typename T>
static std::vector<std::unique_ptr<BaseLayer>> createLayers(runtime::DecodingMode const& mode,
    DecoderDomain const& decodingDomain, cudaStream_t stream,
    std::shared_ptr<tensorrt_llm::common::IAllocator> allocator)
{
    std::vector<std::unique_ptr<BaseLayer>> layers;
    auto layerTypes = createDecodingLayerTypes(mode);
    TLLM_CHECK_WITH_INFO(layerTypes.size() && layerTypes[0] == DecodingLayers_t::PENALTY_LAYER,
        "Penalty layer is required to be the first layer for any decoder configuration");
    for (auto&& type : layerTypes)
    {
        std::unique_ptr<BaseLayer> layer;
        switch (type)
        {
        case DecodingLayers_t::PENALTY_LAYER:
            layer = std::make_unique<PenaltyLayer<T>>(mode, decodingDomain, stream, allocator);
            break;

        case DecodingLayers_t::BAN_WORDS_LAYER:
            layer = std::make_unique<BanWordsLayer<T>>(mode, decodingDomain, stream, allocator);
            break;

        case DecodingLayers_t::DECODING_LAYER:
            layer = std::make_unique<DecodingLayer<T>>(mode, decodingDomain, stream, allocator);
            break;

        case DecodingLayers_t::STOP_CRITERIA_LAYER:
            layer = std::make_unique<StopCriteriaLayer<T>>(mode, decodingDomain, stream, allocator);
            break;

        default: TLLM_CHECK_WITH_INFO(false, "Unknown DecodingLayers_t"); break;
        }
        layers.push_back(std::move(layer));
    }
    return layers;
}
} // namespace tensorrt_llm::layers
