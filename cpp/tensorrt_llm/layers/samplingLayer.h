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

#pragma once

#include <curand_kernel.h>

#include "tensorrt_llm/common/tensor.h"
#include "tensorrt_llm/layers/baseLayer.h"
#include "tensorrt_llm/layers/decodingParams.h"
#include "tensorrt_llm/layers/samplingParams.h"
#include "tensorrt_llm/layers/topKSamplingLayer.h"
#include "tensorrt_llm/layers/topPSamplingLayer.h"
#include "tensorrt_llm/runtime/common.h"
#include "tensorrt_llm/runtime/decodingMode.h"

namespace tc = tensorrt_llm::common;

namespace tensorrt_llm
{
namespace layers
{

//! \brief Top class for sampling layers.
//! It sets up and executes TopKSamplingLayer and TopPSamplingLayer samplings
template <typename T>
class SamplingLayer : public BaseLayer
{
public:
    using Base = BaseLayer;

    SamplingLayer(runtime::DecodingMode const& mode, DecoderDomain const& decoderDomain, cudaStream_t stream,
        std::shared_ptr<tensorrt_llm::common::IAllocator> allocator);

    ~SamplingLayer() override = default;

    void setup(runtime::SizeType32 batchSize, runtime::SizeType32 beamWidth, runtime::SizeType32 const* batchSlots,
        std::shared_ptr<BaseSetupParams> setupParams) override;

    void forward(std::shared_ptr<BaseOutputParams> outputs, std::shared_ptr<BaseInputParams> inputs) override;

private:
    using Base::mWorkspaceSize;
    using Base::mAllocatedSize;

    using Base::mStream;
    using Base::mAllocator;

    using Base::mDecoderDomain;

    runtime::DecodingMode mDecodingMode;

    void* mSamplingWorkspaceDevice{nullptr};
    curandState_t* mCurandStatesDevice{nullptr};
    uint64_t* mRandomSeedsDevice{nullptr};

    bool* mSkipDecodeDevice{nullptr};

    bool* mSkipDecodeHost{nullptr};
    bool mSkipAny{false};

    std::vector<std::unique_ptr<BaseLayer>> mSamplingLayers;

private:
    void allocateBuffer(runtime::SizeType32 batchSize);
    void freeBuffer();
};

} // namespace layers
} // namespace tensorrt_llm
