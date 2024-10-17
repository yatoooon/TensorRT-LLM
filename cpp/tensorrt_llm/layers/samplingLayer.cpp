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

#include "tensorrt_llm/layers/samplingLayer.h"
#include "tensorrt_llm/common/cudaUtils.h"
#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/kernels/decodingCommon.h"
#include "tensorrt_llm/kernels/samplingTopKKernels.h"

#include <algorithm>

using namespace tensorrt_llm::common;
using namespace tensorrt_llm::kernels;
using namespace tensorrt_llm::runtime;

namespace tensorrt_llm
{
namespace layers
{
template <typename T>
SamplingLayer<T>::SamplingLayer(DecodingMode const& mode, DecoderDomain const& decoderDomain, cudaStream_t stream,
    std::shared_ptr<IAllocator> allocator)
    : BaseLayer(decoderDomain, stream, std::move(allocator))
    , mDecodingMode(mode)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    TLLM_CHECK_WITH_INFO(!mDecodingMode.isBeamSearch(), "SamplingLayer does not support Beam search mode");
    TLLM_CHECK_WITH_INFO(mDecodingMode.isTopKorTopP(), "SamplingLayer requires TopK nor TopP mode");
    if (mDecodingMode.isTopK())
    {
        mSamplingLayers.emplace_back(std::make_unique<TopKSamplingLayer<T>>(decoderDomain, mStream, mAllocator));
    }

    if (mDecodingMode.isTopP())
    {
        mSamplingLayers.emplace_back(
            std::make_unique<TopPSamplingLayer<T>>(decoderDomain, mStream, mAllocator, /* deterministic */ true));
    }

    allocateBuffer(decoderDomain.getMaxBatchSize());

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void SamplingLayer<T>::allocateBuffer(SizeType32 batchSize)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    mWorkspaceSize = 0;
    for (auto&& layer : mSamplingLayers)
    {
        mWorkspaceSize = std::max(mWorkspaceSize, layer->getWorkspaceSize());
    }

    std::array<size_t, 4> deviceBufferSizes;
    deviceBufferSizes[0] = sizeof(curandState_t) * batchSize;
    deviceBufferSizes[1] = sizeof(uint64_t) * batchSize;
    deviceBufferSizes[2] = sizeof(bool) * batchSize;
    deviceBufferSizes[3] = mWorkspaceSize;

    mCurandStatesDevice = mAllocator->reMalloc(mCurandStatesDevice, deviceBufferSizes[0], false);
    mRandomSeedsDevice = mAllocator->reMalloc(mRandomSeedsDevice, deviceBufferSizes[1], false);
    mSkipDecodeDevice = mAllocator->reMalloc(mSkipDecodeDevice, deviceBufferSizes[2], false);
    mSamplingWorkspaceDevice = mAllocator->reMalloc(mSamplingWorkspaceDevice, deviceBufferSizes[3], false);

    auto const bytesAllocated = std::accumulate(deviceBufferSizes.begin(), deviceBufferSizes.end(), 0);
    TLLM_LOG_DEBUG("SamplingLayer allocated %d bytes on GPU", bytesAllocated);

    mAllocatedSize = bytesAllocated;
    for (auto&& layer : mSamplingLayers)
    {
        mAllocatedSize += layer->getAllocatedSize();
    }

    // host buffers.
    mSkipDecodeHost = (bool*) std::realloc(mSkipDecodeHost, sizeof(bool) * batchSize);
    TLLM_CHECK(mSkipDecodeHost != nullptr);

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void SamplingLayer<T>::freeBuffer()
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    mAllocator->free((void**) (&mCurandStatesDevice));
    mAllocator->free((void**) (&mRandomSeedsDevice));
    mAllocator->free((void**) (&mSkipDecodeDevice));
    mAllocator->free((void**) (&mSamplingWorkspaceDevice));
    std::free(mSkipDecodeHost);

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void SamplingLayer<T>::setup(SizeType32 batchSize, SizeType32 beamWidth, SizeType32 const* batchSlots,
    std::shared_ptr<BaseSetupParams> baseSetupParams)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto setupParams = std::dynamic_pointer_cast<SamplingSetupParams>(baseSetupParams);

    // If runtime argument has single random seed, using this random seed to
    // initialize the random table of all sentences. If the argument has
    // [batchSize] random seeds, initializing the random table by different
    // random seeds respectively. If no random seed, initialize the random table
    // of all sentences by 0 directly.
    if (setupParams->randomSeed)
    {
        if (setupParams->randomSeed->size() == 1)
        {
            invokeCurandInitialize(
                mCurandStatesDevice, batchSlots, batchSize, setupParams->randomSeed->front(), mStream);
            sync_check_cuda_error();
        }
        else
        {
            TLLM_CHECK_WITH_INFO(setupParams->randomSeed->size() == batchSize, "Random seed vector size mismatch.");
            cudaAutoCpy(mRandomSeedsDevice, setupParams->randomSeed->data(), batchSize, mStream);
            invokeCurandBatchInitialize(mCurandStatesDevice, batchSlots, batchSize, mRandomSeedsDevice, mStream);
            sync_check_cuda_error();
        }
    }
    else
    {
        // Initialize curand states using the default seed 0.
        invokeCurandInitialize(mCurandStatesDevice, batchSlots, batchSize, 0, mStream);
    }

    for (auto&& layer : mSamplingLayers)
    {
        layer->setup(batchSize, beamWidth, batchSlots, setupParams);
    }

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void SamplingLayer<T>::forward(
    std::shared_ptr<BaseOutputParams> baseOutputs, std::shared_ptr<BaseInputParams> baseInputs)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto inputs = std::dynamic_pointer_cast<SamplingInputParams>(baseInputs);
    auto outputs = std::dynamic_pointer_cast<SamplingOutputParams>(baseOutputs);

    auto const batchSize = inputs->logits.shape[0];

    auto logits = inputs->logits.template getPtr<T>();
    auto endIds = inputs->end_ids.template getPtr<int const>();
    auto batchSlots = inputs->batch_slots ? inputs->batch_slots->template getPtr<int const>() : nullptr;
    float* cumLogProbs = (outputs->cum_log_probs) ? outputs->cum_log_probs->template getPtr<float>() : nullptr;
    float* outputLogProbs = (outputs->output_log_probs) ? outputs->output_log_probs->template getPtr<float>() : nullptr;

    FinishedState* finishedInput = (inputs->finished)
        ? reinterpret_cast<FinishedState*>(inputs->finished->template getPtr<FinishedState::UnderlyingType>())
        : nullptr;

    auto const skipTopP = !mDecodingMode.isTopP();

    // Compute probabilities either for TopP or if cumLogProbs or outputLogProbs are specified
    bool const skipSoftMax = skipTopP && cumLogProbs == nullptr && outputLogProbs == nullptr;

    inputs->curand_states = mCurandStatesDevice;
    inputs->sampling_workspace = mSamplingWorkspaceDevice;
    inputs->probs_computed = !skipSoftMax;

    invokeAddBiasSoftMax(logits, (T**) nullptr, logits, (T*) (nullptr), endIds, finishedInput, batchSlots, batchSize,
        mDecoderDomain.getMaxBatchSize(), /* bw */ 1, mDecoderDomain.getVocabSize(),
        mDecoderDomain.getVocabSizePadded(), skipSoftMax, /* batchSlotLogits */ false, mStream);
    sync_check_cuda_error();

    for (auto&& layer : mSamplingLayers)
    {
        layer->forward(baseOutputs, baseInputs);
    }

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template class SamplingLayer<float>;
template class SamplingLayer<half>;

} // namespace layers
} // namespace tensorrt_llm
