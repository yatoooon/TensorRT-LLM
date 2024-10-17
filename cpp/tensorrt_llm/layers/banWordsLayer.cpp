/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (c) 2021, NAVER Corp.  Authored by CLOVA.
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

#include "tensorrt_llm/layers/banWordsLayer.h"
#include "tensorrt_llm/common/cudaUtils.h"
#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/kernels/banBadWords.h"
#include "tensorrt_llm/kernels/banRepeatNgram.h"

#include <algorithm>

using namespace tensorrt_llm::common;
using namespace tensorrt_llm::kernels;
using namespace tensorrt_llm::runtime;

namespace tensorrt_llm
{
namespace layers
{

template <typename T>
BanWordsLayer<T>::BanWordsLayer(DecodingMode const& mode, DecoderDomain const& decoderDomain, cudaStream_t stream,
    std::shared_ptr<IAllocator> allocator)
    : BaseLayer(decoderDomain, stream, std::move(allocator))
    , mDecodingMode(mode)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void BanWordsLayer<T>::setup(SizeType32 batchSize, SizeType32 beamWidth, SizeType32 const* batchSlots,
    std::shared_ptr<BaseSetupParams> setupParams)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void BanWordsLayer<T>::banRepeatNGrams(Tensor& logits, std::shared_ptr<DynamicDecodeOutputParams> const& outputs,
    std::shared_ptr<DynamicDecodeInputParams> const& inputs, SizeType32 const* batchSlots, SizeType32 batchSize,
    SizeType32 beamWidth, SizeType32 maxSeqLen, SizeType32 vocabSizePadded, cudaStream_t stream)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    auto const max_step = inputs->step;
    if (inputs->no_repeat_ngram_size)
    {
        SizeType32 const* noRepeatNgramSizeBuf
            = inputs->no_repeat_ngram_size.value().template getPtr<SizeType32 const>();

        invokeBanRepeatNgram(logits.template getPtr<T>(), outputs->output_ids_ptr.template getPtr<TokenIdType const*>(),
            reinterpret_cast<FinishedState*>(
                inputs->finished.value_or(Tensor{}).template getPtr<FinishedState::UnderlyingType>()),
            outputs->parent_ids_ptr.template getPtr<SizeType32 const*>(), batchSlots,
            outputs->sequence_length->template getPtr<SizeType32>(), batchSize, beamWidth, maxSeqLen,
            inputs->no_repeat_ngram_size.value().template getPtr<SizeType32 const>(), vocabSizePadded, max_step,
            stream);
    }
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void BanWordsLayer<T>::banBadWords(Tensor& logits, std::shared_ptr<DynamicDecodeOutputParams> const& outputs,
    std::shared_ptr<DynamicDecodeInputParams> const& inputs, SizeType32 const* batchSlots, SizeType32 batchSize,
    SizeType32 beamWidth, SizeType32 maxSeqLen, SizeType32 vocabSizePadded, cudaStream_t stream)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    auto const maxBadWordsLength = inputs->max_bad_words_len;
    if (maxBadWordsLength)
    {
        auto const** badWordsPtr = inputs->bad_words_ptr->template getPtr<TokenIdType const*>();
        auto const* badWordsLens = inputs->bad_words_lengths->template getPtr<SizeType32>();

        invokeBanBadWords((T*) logits.template getPtr<T>(),
            outputs->output_ids_ptr.template getPtr<TokenIdType const*>(),
            beamWidth > 1 ? outputs->parent_ids_ptr.template getPtr<SizeType32 const*>() : nullptr, batchSlots,
            batchSize, beamWidth, badWordsPtr, badWordsLens, maxBadWordsLength, vocabSizePadded,
            outputs->sequence_length->template getPtr<SizeType32>(), maxSeqLen, stream);
    }
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void BanWordsLayer<T>::forward(
    std::shared_ptr<BaseOutputParams> baseOutputs, std::shared_ptr<BaseInputParams> baseInputs)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto inputs = std::dynamic_pointer_cast<DynamicDecodeInputParams>(baseInputs);
    auto outputs = std::dynamic_pointer_cast<DynamicDecodeOutputParams>(baseOutputs);

    SizeType32 batchSize{0};
    SizeType32 beamWidth{0};
    SizeType32 vocabSize{0};
    auto const maxSeqLen = outputs->output_ids.shape[outputs->output_ids.shape.size() - 1];
    auto batchSlots = inputs->batch_slots ? inputs->batch_slots->template getPtr<SizeType32 const>() : nullptr;
    if (inputs->logits)
    {
        auto const& logitsShape = inputs->logits->shape;
        TLLM_CHECK(logitsShape.size() == 3 || logitsShape.size() == 4);
        batchSize = logitsShape[0];
        auto const idxOffset = logitsShape.size() - 3;
        beamWidth = logitsShape[idxOffset + 1];
        vocabSize = logitsShape[idxOffset + 2];
    }
    else
    {
        TLLM_CHECK(inputs->logits_vec->size());
        auto const& logitsShape = inputs->logits_vec.value()[0].shape;
        TLLM_CHECK(logitsShape.size() == 3 || logitsShape.size() == 4);
        auto const idxOffset = logitsShape.size() - 3;
        batchSize = inputs->logits_vec->size();
        beamWidth = logitsShape[idxOffset + 1];
        vocabSize = logitsShape[idxOffset + 2];
    }

    banRepeatNGrams(
        inputs->logits.value(), outputs, inputs, batchSlots, batchSize, beamWidth, maxSeqLen, vocabSize, mStream);
    banBadWords(
        inputs->logits.value(), outputs, inputs, batchSlots, batchSize, beamWidth, maxSeqLen, vocabSize, mStream);

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template class BanWordsLayer<float>;
template class BanWordsLayer<half>;

} // namespace layers
} // namespace tensorrt_llm
