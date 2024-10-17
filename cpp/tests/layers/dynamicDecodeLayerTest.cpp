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

#include "tests/layers/dynamicDecodeLayerTest.h"
#include <algorithm>

namespace tensorrt_llm::tests::layers::sampling
{

// TODO(nkorobov):
// Add tests for
// - finished states
// - finished sum
// - max length
// - repeat n grams
// - padded vocab
// - beam search

using namespace tensorrt_llm::runtime;
using namespace tensorrt_llm::layers;
using namespace tensorrt_llm::common;

namespace tk = tensorrt_llm::kernels;
namespace tcc = tensorrt_llm::common::conversion;
namespace trk = tensorrt_llm::runtime::kernels;

constexpr float EPSILON = 1e-20f;

inline bool almostEqual(float a, float b, float atol = 1e-5, float rtol = 1e-8)
{
    // Params: a = value to compare and b = reference
    // This function follows implementation of numpy.isclose(), which checks
    //   abs(a - b) <= (atol + rtol * abs(b)).
    // Note that the inequality above is asymmetric where b is considered as
    // a reference value. To account into both absolute/relative errors, it
    // uses absolute tolerance and relative tolerance at the same time. The
    // default values of atol and rtol borrowed from numpy.isclose(). For the
    // case of nan value, the result will be true.
    if (isnan(a) && isnan(b))
    {
        return true;
    }

    if (isinf(a) && isinf(b))
    {
        return true;
    }
    return fabs(a - b) <= (atol + rtol * fabs(b));
}

template <typename T>
bool compareValues(T* out, T* ref, size_t size)
{
    bool isFp32 = sizeof(T) == 4;
    float atol = isFp32 ? 1e-4f : 1e-3f;
    float rtol = isFp32 ? 1e-2f : 1e-1f;

    size_t failures = 0;
    float relativeGap = 0.0f;

    for (size_t i = 0; i < size; ++i)
    {
        // The values for the output and the reference.
        float a = (float) out[i];
        float b = (float) ref[i];

        bool ok = almostEqual(a, b, atol, rtol);
        // Print the error.
        if (!ok && failures < 4)
        {
            TLLM_LOG_DEBUG(">> invalid result for i=%lu:", i);
            TLLM_LOG_DEBUG(">>    found......: %10.6f", a);
            TLLM_LOG_DEBUG(">>    expected...: %10.6f", b);
            TLLM_LOG_DEBUG(">>    error......: %.6f", fabsf(a - b));
            TLLM_LOG_DEBUG(">>    tol........: %.6f", atol + rtol * fabs(b));
        }
        // Update the number of failures.
        failures += ok ? 0 : 1;
        // Update the relative gap.
        relativeGap += fabsf(a - b) / (fabsf(b) + EPSILON);
    }

    relativeGap /= size;

    // Allow not matched up to 0% elements.
    size_t tolFailures = (size_t) (0.0 * size);
    TLLM_LOG_DEBUG("check... : %-50s (failures: %.2f%% atol: %.2e rtol: %.2e rel_gap: %.2e%%)",
        failures <= tolFailures ? "....OK" : "FAILED", 100. * failures / size, atol, rtol, 100. * relativeGap);
    return failures <= tolFailures;
}

template bool compareValues(float* out, float* ref, size_t size);
template bool compareValues(half* out, half* ref, size_t size);

template <typename T>
void DynamicDecodeLayerTest<T>::SetUp()
{
    mStream = std::make_shared<tensorrt_llm::runtime::CudaStream>();
    mBufferManager = std::make_shared<tensorrt_llm::runtime::BufferManager>(mStream);

    mAllocator = std::make_shared<tensorrt_llm::common::CudaAllocator>(*mBufferManager);
}

template <typename T>
void DynamicDecodeLayerTest<T>::allocateData(TestSamplingParams const& params, TokenIdType endId)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    mEndId = endId == -1 ? mVocabSize - 1 : endId;
    mUseMedusa = params.useMedusa;
    mMaxTokensPerStep = mUseMedusa ? mMaxOutputLen - mMaxInputLen : 1;

    auto const decodingMode = [this]()
    {
        if (this->mBeamWidth == 1)
        {
            if (this->mUseMedusa)
            {
                return DecodingMode::Medusa();
            }
            else
            {
                return DecodingMode::TopKTopP();
            }
        }
        else
        {
            return DecodingMode::BeamSearch();
        }
    }();

    auto const decodingDomain = tensorrt_llm::layers::DecoderDomain(
        mMaxBatchSize, mBeamWidth, mVocabSize, mVocabSizePadded, mMaxTokensPerStep, params.maxNumMedusaHeads);

    mDecodeLayer = std::make_unique<tensorrt_llm::layers::DynamicDecodeLayer<T>>(
        decodingMode, decodingDomain, mStream->get(), mAllocator);

    auto const dataType = TRTDataType<T>::value;

    mLogitsDevice = mBufferManager->gpu(
        ITensor::makeShape({mBatchSize, mMaxTokensPerStep, mBeamWidth, mVocabSizePadded}), dataType);
    mRuntimeLogitsHost
        = BufferManager::pinned(ITensor::makeShape({mBatchSize, mBeamWidth, mVocabSizePadded}), dataType);

    mSeqLengthsDevice = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mContextLengthDevice = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mFinishedDevice = mBufferManager->gpu(
        ITensor::makeShape({mMaxBatchSize}), TRTDataType<tk::FinishedState::UnderlyingType>::value);
    mFinishedSumDevice = BufferManager::pinned(ITensor::makeShape({1}), nvinfer1::DataType::kFLOAT);
    mOutputIdsDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize, mBeamWidth, mMaxSeqLen}), nvinfer1::DataType::kINT32);
    mNewTokens
        = BufferManager::pinned(ITensor::makeShape({mMaxTokensPerStep, mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mEndIdsDevice = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);

    mEmbeddingBiasHost = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize, mVocabSizePadded}), dataType);
    mEmbeddingBiasDevice = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize, mVocabSizePadded}), dataType);

    mRefLogProbsHost
        = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize, mMaxSeqLen}), nvinfer1::DataType::kFLOAT);
    mOutputLogProbsDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize, mMaxSeqLen}), nvinfer1::DataType::kFLOAT);
    mOutputLogProbsTiledDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxSeqLen, mMaxBatchSize}), nvinfer1::DataType::kFLOAT);

    mCumLogProbsDevice = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kFLOAT);

    mMaxBadWordsLen = getMaxWordsLen(params.badWords);
    mMaxStopWordsLen = getMaxWordsLen(params.stopWords);

    mBadWords
        = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize, 2, mMaxBadWordsLen}), nvinfer1::DataType::kINT32);
    mBadWordsLens = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mBadWordsPtrs = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT64);

    mStopWords
        = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize, 2, mMaxStopWordsLen}), nvinfer1::DataType::kINT32);
    mStopWordsLens = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mStopWordsPtrs = BufferManager::pinned(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT64);

    mBatchSlots = BufferManager::pinned(ITensor::makeShape({mBatchSize}), nvinfer1::DataType::kINT32);

    if (mUseMedusa)
    {
        allocateMedusaData(params);
    }

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void DynamicDecodeLayerTest<T>::allocateMedusaData(TestSamplingParams const& params)
{
    auto const dataType = TRTDataType<T>::value;
    mMaxMedusaHeads = params.maxNumMedusaHeads.value();
    mPathsDevice = mBufferManager->gpu(
        ITensor::makeShape({mMaxBatchSize, mMaxTokensPerStep, mMaxMedusaHeads + 1}), nvinfer1::DataType::kINT32);
    mAcceptedLengths = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mMedusaLogitsDevice = BufferManager::pinned(
        ITensor::makeShape({mMaxMedusaHeads, mMaxBatchSize, mMaxTokensPerStep, mVocabSizePadded}), dataType);
    mNextDraftTokensDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize, mMaxTokensPerStep - 1}), nvinfer1::DataType::kINT32);
    mTokensPerStepDevice = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize}), nvinfer1::DataType::kINT32);
    mTreeIdsDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize, mMaxTokensPerStep - 1}), nvinfer1::DataType::kINT32);
    mAcceptedLengthCumSumDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize + 1}), nvinfer1::DataType::kINT32);
    mPackedPathsDevice
        = mBufferManager->gpu(ITensor::makeShape({mMaxBatchSize * mMaxMedusaHeads}), nvinfer1::DataType::kINT32);
}

template <typename T>
void DynamicDecodeLayerTest<T>::setup(uint64_t seed, TestSamplingParams const& params)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto const dataType = TRTDataType<T>::value;

    // clang-format off

    // prob = (0.0, 0.0, 0.0, 0.0, 0.4, 0.3, 0.2, 0.1, 0.0)
    mTestLogitsInit = {
            -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, -0.9163, -1.2040, -1.6094, -2.3026, -FLT_MAX, // step 0
            -0.9163, -1.2040, -1.6094, -2.3026, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, // step 1
            -FLT_MAX, -FLT_MAX, -0.9163, -1.2040, -1.6094, -2.3026, -FLT_MAX, -FLT_MAX, -FLT_MAX, // step 2
            -0.9163, -1.2040, -1.6094, -2.3026, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX  // step 3
    };

    // clang-format on

    trk::invokeFill(*mSeqLengthsDevice, SizeType32{0}, *mStream);
    trk::invokeFill(*mContextLengthDevice, SizeType32{0}, *mStream);
    trk::invokeFill(*mFinishedDevice, uint8_t{0}, *mStream);
    trk::invokeFill(*mOutputIdsDevice, TokenIdType{0}, *mStream);
    trk::invokeFill(*mEmbeddingBiasDevice, T{0.0f}, *mStream);
    trk::invokeFill(*mCumLogProbsDevice, float{0.0f}, *mStream);
    trk::invokeFill(*mOutputLogProbsDevice, float{0.0f}, *mStream);
    trk::invokeFill(*mOutputLogProbsTiledDevice, float{0.0f}, *mStream);
    trk::invokeFill(*mRefLogProbsHost, float{0.0f}, *mStream);
    trk::invokeFill(*mEndIdsDevice, TokenIdType{mEndId}, *mStream);

    auto batchSlotsPtr = bufferCast<SizeType32>(*mBatchSlots);
    for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
    {
        batchSlotsPtr[bi] = 2 * bi;
    }

    if (params.useBias)
    {
        auto embeddingBiasHostPtr = bufferCast<T>(*mEmbeddingBiasHost);
        for (SizeType32 bi = 0; bi < mMaxBatchSize; bi++)
        {
            for (SizeType32 vi = 0; vi < mVocabSizePadded; vi++)
            {
                embeddingBiasHostPtr[bi * mVocabSizePadded + vi] = 2 <= vi && vi < 6 ? T{2.0f} : T{0.0f};
            }
        }
        mBufferManager->copy(*mEmbeddingBiasHost, *mEmbeddingBiasDevice);
    }

    mLogitsVec.resize(mBatchSize);
    for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
    {
        auto logitsSlice = ITensor::slice(mLogitsDevice, bi, 1);
        mLogitsVec[bi] = tcc::toTllmTensor(*logitsSlice);
    }

    if (mUseMedusa)
    {
        auto const maxMedusaHeads = params.maxNumMedusaHeads.value();

        trk::invokeFill(*mPathsDevice, SizeType32{-1}, *mStream);
        trk::invokeFill(*mAcceptedLengths, SizeType32{0}, *mStream);
        trk::invokeFill(*mNextDraftTokensDevice, TokenIdType{mEndId}, *mStream);
        trk::invokeFill(*mTokensPerStepDevice, SizeType32{0}, *mStream);
        trk::invokeFill(*mTreeIdsDevice, SizeType32{0}, *mStream);

        auto const logitsHost
            = ITensor::wrap(mTestLogitsInit, ITensor::makeShape({mMaxTokensPerStep, mVocabSizePadded}));
        for (SizeType32 hi = 0; hi < maxMedusaHeads; ++hi)
        {
            TensorPtr logitsHeadDeviceView = ITensor::slice(mMedusaLogitsDevice, hi, 1);
            logitsHeadDeviceView->squeeze(0);
            for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
            {
                TensorPtr logitsHeadBatchDeviceView = ITensor::slice(logitsHeadDeviceView, bi, 1);
                mBufferManager->copy(*logitsHost, *logitsHeadBatchDeviceView);
            }
        }

        auto paths = params.paths.value();
        for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
        {
            auto const numPaths = static_cast<SizeType32>(paths[bi].size() / (maxMedusaHeads + 1));
            auto const pathsHost = ITensor::wrap(paths[bi], ITensor::makeShape({1, numPaths, maxMedusaHeads + 1}));
            TensorPtr pathsDeviceSlice = ITensor::slice(mPathsDevice, batchSlotsPtr[bi], 1);
            pathsDeviceSlice->squeeze(0);
            TensorPtr pathsNumPathsDeviceSlice = ITensor::slice(pathsDeviceSlice, 0, numPaths);
            pathsNumPathsDeviceSlice->unsqueeze(0);
            mBufferManager->copy(*pathsHost, *pathsNumPathsDeviceSlice);
        }

        auto tokensPerStep = params.tokensPerStep.value();
        for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
        {
            TensorPtr tokensPerStepDeviceSlice = ITensor::slice(mTokensPerStepDevice, batchSlotsPtr[bi], 1);
            trk::invokeFill(*tokensPerStepDeviceSlice, SizeType32{tokensPerStep[bi]}, *mStream);
        }

        auto outputIds = params.outputIds.value();
        for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
        {
            auto const outputIdsBatchHost = ITensor::wrap(outputIds[bi], ITensor::makeShape({mMaxTokensPerStep - 1}));

            auto outputIdsDevice = ITensor::slice(mNextDraftTokensDevice, batchSlotsPtr[bi], 1);
            mBufferManager->copy(*outputIdsBatchHost, *outputIdsDevice);
        }
    }

    auto setupParams = std::make_shared<DynamicDecodeSetupParams>();
    setupParams->penaltyParams.temperature
        = params.temperatures.size() ? std::make_optional<std::vector<float>>(params.temperatures) : std::nullopt;
    setupParams->penaltyParams.repetitionPenalty = params.repetitionPenalties.size()
        ? std::make_optional<std::vector<float>>(params.repetitionPenalties)
        : std::nullopt;
    setupParams->penaltyParams.presencePenalty = params.presencePenalties.size()
        ? std::make_optional<std::vector<float>>(params.presencePenalties)
        : std::nullopt;
    setupParams->penaltyParams.frequencyPenalty = params.frequencyPenalties.size()
        ? std::make_optional<std::vector<float>>(params.frequencyPenalties)
        : std::nullopt;
    setupParams->penaltyParams.minLength
        = params.minLengths.size() ? std::make_optional<std::vector<SizeType32>>(params.minLengths) : std::nullopt;
    setupParams->randomSeed = std::make_optional<std::vector<uint64_t>>({seed});
    setupParams->samplingParams.runtime_top_k
        = params.topKs.size() ? std::make_optional<std::vector<SizeType32>>(params.topKs) : std::nullopt;
    setupParams->samplingParams.runtime_top_p
        = params.topPs.size() ? std::make_optional<std::vector<float>>(params.topPs) : std::nullopt;
    setupParams->samplingParams.top_p_decay
        = params.decay.size() ? std::make_optional<std::vector<float>>(params.decay) : std::nullopt;
    setupParams->samplingParams.top_p_min
        = params.minTopP.size() ? std::make_optional<std::vector<float>>(params.minTopP) : std::nullopt;
    setupParams->samplingParams.top_p_reset_ids
        = params.topPResetIds.size() ? std::make_optional<std::vector<TokenIdType>>(params.topPResetIds) : std::nullopt;
    setupParams->samplingParams.normalize_log_probs = {false};

    setupParams->medusaParams.topKMedusaHeads = params.topKMedusaHeads;

    initXWordsTensors(batchSlotsPtr, bufferCast<SizeType32>(*mBadWords),
        reinterpret_cast<SizeType32**>(bufferCast<int64_t>(*mBadWordsPtrs)), bufferCast<SizeType32>(*mBadWordsLens),
        mMaxBadWordsLen, params.badWords);
    initXWordsTensors(batchSlotsPtr, bufferCast<SizeType32>(*mStopWords),
        reinterpret_cast<SizeType32**>(bufferCast<int64_t>(*mStopWordsPtrs)), bufferCast<SizeType32>(*mStopWordsLens),
        mMaxStopWordsLen, params.stopWords);

    mDecodeLayer->setup(mBatchSize, mBeamWidth, batchSlotsPtr, setupParams);

    mStream->synchronize();

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
SizeType32 DynamicDecodeLayerTest<T>::getMaxWordsLen(
    std::vector<std::vector<std::vector<SizeType32>>> const& inputWords)
{
    SizeType32 maxWordsLen = 0;
    for (auto const& batchWords : inputWords)
    {
        SizeType32 wordsLen = 0;
        for (auto const& words : batchWords)
        {
            wordsLen += words.size();
        }
        if (wordsLen == batchWords.size())
        {
            wordsLen += 1;
        }
        maxWordsLen = std::max(maxWordsLen, wordsLen);
    }
    return maxWordsLen;
}

template <typename T>
void DynamicDecodeLayerTest<T>::initXWordsTensors(SizeType32* batchSlotsPtr, SizeType32* wordsData,
    SizeType32** wordsPtr, SizeType32* wordsLenData, SizeType32 maxWordsLen,
    std::vector<std::vector<std::vector<SizeType32>>> const& inputWords)
{
    std::fill(wordsData, wordsData + mMaxBatchSize * 2 * maxWordsLen, -1);
    for (SizeType32 bi = 0; bi < inputWords.size(); bi++)
    {
        auto const batchSlot = batchSlotsPtr[bi];
        SizeType32 totalLen = 0;
        for (SizeType32 wi = 0; wi < inputWords[bi].size(); ++wi)
        {
            for (SizeType32 si = 0; si < inputWords[bi][wi].size(); ++si)
            {
                wordsData[batchSlot * 2 * maxWordsLen + 0 * maxWordsLen + totalLen + si] = inputWords[bi][wi][si];
            }
            totalLen += inputWords[bi][wi].size();
            // Do not add value if words is empty
            if (totalLen > 0)
            {
                wordsData[batchSlot * 2 * maxWordsLen + 1 * maxWordsLen + wi] = totalLen;
            }
        }
    }

    for (SizeType32 bi = 0; bi < inputWords.size(); bi++)
    {
        auto const batchSlot = batchSlotsPtr[bi];
        wordsPtr[batchSlot] = wordsData + batchSlot * 2 * maxWordsLen;

        wordsLenData[batchSlot] = maxWordsLen;
    }
}

template <typename T>
DynamicDecodeInputParams::MedusaInputs DynamicDecodeLayerTest<T>::createMedusaInputs()
{
    DynamicDecodeInputParams::MedusaInputs medusaInputs;
    auto batchSlots = BufferRange<SizeType32>(*mBatchSlots);
    std::vector<std::vector<tc::Tensor>> medusaLogits(mMaxBatchSize);
    auto const medusaLogitsPtr = bufferCast<T>(*mMedusaLogitsDevice);
    for (SizeType32 bi = 0; bi < mMaxBatchSize; ++bi)
    {
        medusaLogits[bi].resize(mMaxMedusaHeads);
    }
    for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
    {
        for (SizeType32 hi = 0; hi < mMaxMedusaHeads; ++hi)
        {
            TensorPtr logitsHead = ITensor::slice(mMedusaLogitsDevice, hi, 1);
            logitsHead->squeeze(0);
            TensorPtr logitsHeadBatch = ITensor::slice(logitsHead, bi, 1);
            medusaLogits[batchSlots[bi]][hi] = tcc::toTllmTensor(*logitsHeadBatch);
        }
    }

    medusaInputs.medusaPaths = tcc::toTllmTensor(*mPathsDevice);
    medusaInputs.medusaTreeIds = tcc::toTllmTensor(*mTreeIdsDevice);
    medusaInputs.medusaLogits = medusaLogits;
    medusaInputs.medusaCurTokensPerStep = tcc::toTllmTensor(*mTokensPerStepDevice);
    medusaInputs.medusaTargetTokensPerStep = tcc::toTllmTensor(*mTokensPerStepDevice);
    return medusaInputs;
}

template <typename T>
std::shared_ptr<DynamicDecodeInputParams> DynamicDecodeLayerTest<T>::createInputTensors(SizeType32 step)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    constexpr SizeType32 ite = 0;
    auto forwardParams = std::make_shared<DynamicDecodeInputParams>(
        step, ite, mMaxInputLen, mMaxSeqLen, mSinkTokenLength, mBatchSize, tcc::toTllmTensor(*mEndIdsDevice));

    forwardParams->embedding_bias = tcc::toTllmTensor(*mEmbeddingBiasDevice);

    forwardParams->finished = tcc::toTllmTensor(*mFinishedDevice);

    forwardParams->batch_slots = tcc::toTllmTensor(*mBatchSlots);

    if (mUseLogitsVec)
    {
        forwardParams->logits_vec = mLogitsVec;
    }
    else
    {
        forwardParams->logits = tcc::toTllmTensor(*mLogitsDevice);
    }

    forwardParams->bad_words_ptr = tcc::toTllmTensor(*mBadWordsPtrs);
    forwardParams->bad_words_lengths = tcc::toTllmTensor(*mBadWordsLens);
    forwardParams->max_bad_words_len = mMaxBadWordsLen;

    forwardParams->stop_words_ptr = tcc::toTllmTensor(*mStopWordsPtrs);
    forwardParams->stop_words_lengths = tcc::toTllmTensor(*mStopWordsLens);
    forwardParams->max_stop_words_len = mMaxStopWordsLen;

    if (mUseMedusa)
    {
        forwardParams->medusaInputs = createMedusaInputs();
    }

    // TODO(nkorobov): extend to
    // std::optional<tc::Tensor> src_cache_indirection;
    // std::optional<tc::Tensor> sequence_limit_length;
    // std::optional<tc::Tensor> input_lengths;
    // std::optional<tc::Tensor> no_repeat_ngram_size;
    // std::optional<std::vector<tc::Tensor>> logits_vec;

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);

    return forwardParams;
}

template <typename T>
DynamicDecodeOutputParams::MedusaOutputs DynamicDecodeLayerTest<T>::createMedusaOutputs()
{
    DynamicDecodeOutputParams::MedusaOutputs medusaOutputs;
    medusaOutputs.nextDraftTokens = tcc::toTllmTensor(*mNextDraftTokensDevice);

    medusaOutputs.acceptedLengths = tcc::toTllmTensor(*mAcceptedLengths);

    medusaOutputs.acceptedLengthsCumSum = tcc::toTllmTensor(*mAcceptedLengthCumSumDevice);

    medusaOutputs.pathsOffsets = tcc::toTllmTensor(*mPackedPathsDevice);
    return medusaOutputs;
}

template <typename T>
std::shared_ptr<DynamicDecodeOutputParams> DynamicDecodeLayerTest<T>::createOutputTensors()
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    auto outputParams = std::make_shared<DynamicDecodeOutputParams>(tcc::toTllmTensor(*mOutputIdsDevice));

    outputParams->sequence_length = tcc::toTllmTensor(*mSeqLengthsDevice);

    outputParams->finished = tcc::toTllmTensor(*mFinishedDevice);

    outputParams->finished_sum = tcc::toTllmTensor(*mFinishedSumDevice);

    outputParams->newTokens = tcc::toTllmTensor(*mNewTokens);

    if (!mUseMedusa)
    {
        // Output log probs are not supported in Medusa
        outputParams->cum_log_probs = tcc::toTllmTensor(*mCumLogProbsDevice);

        outputParams->output_log_probs = tcc::toTllmTensor(*mOutputLogProbsDevice);

        outputParams->output_log_probs_tiled = tcc::toTllmTensor(*mOutputLogProbsTiledDevice);
    }

    if (mUseMedusa)
    {
        outputParams->medusaOutputs = createMedusaOutputs();
    }

    // TODO(nkorobov): extend to
    // std::optional<tc::Tensor> parent_ids;
    // std::optional<tc::Tensor> tgt_cache_indirection;
    // std::shared_ptr<kernels::BeamHypotheses> beamHypotheses;

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);

    return outputParams;
}

template <typename T>
void DynamicDecodeLayerTest<T>::batchCopy(SizeType32 step)
{
    auto const logitsHost = ITensor::wrap(mTestLogitsInit.data() + step * mVocabSizePadded,
        std::is_same_v<T, float> ? nvinfer1::DataType::kFLOAT : nvinfer1::DataType::kHALF,
        ITensor::makeShape({mMaxTokensPerStep, mVocabSizePadded}));
    for (SizeType32 bi = 0; bi < mBatchSize; ++bi)
    {
        TensorPtr logitsDeviceView = ITensor::slice(mLogitsDevice, bi, 1);
        logitsDeviceView->squeeze(0);
        mBufferManager->copy(*logitsHost, *logitsDeviceView);
    }
    mLogitsRefHost = mBufferManager->copyFrom(*mLogitsDevice, tensorrt_llm::runtime::MemoryType::kCPU);
}

template <typename T>
bool DynamicDecodeLayerTest<T>::checkResult(TokenIdType* outputIds,
    std::vector<std::set<TokenIdType>> const& expectedIds, SizeType32* seqLens, SizeType32 leadingDim,
    SizeType32 stride, SizeType32 step, bool outputIdsTransposed, SizeType32 strideTransposed)
{
    SizeType32 failures = 0;
    auto const batchSlotsPtr = bufferCast<SizeType32>(*mBatchSlots);
    for (SizeType32 i = 0; i < leadingDim * stride; ++i)
    {
        auto const s = i / stride;
        auto const b = i % stride;
        auto const batchSlot = batchSlotsPtr[b];
        if (seqLens[batchSlot] <= step + s)
        {
            continue;
        }
        auto const& expts = expectedIds.at(i + step * stride);
        auto const outputIdIdx = outputIdsTransposed ? s * strideTransposed + batchSlot : batchSlot * leadingDim + s;
        auto const outputId = outputIds[outputIdIdx];
        if (expts.count(outputId) == 0)
        {
            if (failures < 10)
            {
                std::stringstream ss;
                ss << " - Fail "
                   << " (step=" << s << ", batch=" << b << ") "
                   << "actual=" << outputId << ", expected";
                for (auto const& expt : expts)
                {
                    ss << " " << expt;
                }
                TLLM_LOG_DEBUG("%s", ss.str().c_str());
            }
            ++failures;
        }
    }
    TLLM_LOG_DEBUG(
        "check...%6s : failures: %d / %d", failures == 0 ? "....OK" : "FAILED", failures, leadingDim * stride);
    return failures == 0;
}

template <typename T>
void DynamicDecodeLayerTest<T>::fillRefLogits(
    SizeType32 const* seqLenHost, std::vector<std::set<TokenIdType>> const& expectedOutputIds, SizeType32 step)
{
    auto const batchSlotsPtr = bufferCast<SizeType32>(*mBatchSlots);
    auto const runtimeLogitsHost = bufferCast<T>(*mRuntimeLogitsHost);
    for (SizeType32 bi = 0; bi < mBatchBeam; ++bi)
    {
        auto const batchSlot = batchSlotsPtr[bi];
        if (seqLenHost[batchSlot] <= step)
        {
            continue;
        }
        auto& expectedSet = expectedOutputIds[step * mBatchBeam + bi];
        TLLM_CHECK(expectedSet.size() == 1);
        auto expectedToken = *expectedSet.begin();
        bufferCast<float>(*mRefLogProbsHost)[batchSlot * mMaxSeqLen + step]
            = logf(runtimeLogitsHost[bi * mVocabSizePadded + expectedToken]);
    }
}

template <typename T>
void DynamicDecodeLayerTest<T>::runTestImpl(
    std::vector<std::set<TokenIdType>> const& expectedOutputIds, TestSamplingParams const& params, TokenIdType endId)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    bool greedySearch
        = std::all_of(expectedOutputIds.begin(), expectedOutputIds.end(), [](auto v) { return v.size() == 1; });
    for (uint64_t seed = 0; seed < mMaxSeed; ++seed)
    {
        setup(seed, params);

        auto step = mMaxInputLen;
        auto inputTensors = createInputTensors(step);
        auto outputTensors = createOutputTensors();

        for (step = mMaxInputLen; step < mMaxOutputLen; step += mMaxTokensPerStep)
        {
            // Reset by the test value since the sampling layer internally update the logit buffer.
            batchCopy(step);
            if (mUseLogitsVec)
            {
                inputTensors->logits_vec = mLogitsVec;
                inputTensors->logits = std::nullopt;
            }
            else
            {
                inputTensors->logits = tcc::toTllmTensor(*mLogitsDevice);
                inputTensors->logits_vec = std::nullopt;
            }
            inputTensors->step = step;
            mDecodeLayer->forward(outputTensors, inputTensors);
            mStream->synchronize();
            auto const newTokensHost = mBufferManager->copyFrom(*mNewTokens, tensorrt_llm::runtime::MemoryType::kCPU);
            auto const seqLenHost
                = mBufferManager->copyFrom(*mSeqLengthsDevice, tensorrt_llm::runtime::MemoryType::kCPU);
            auto const logitsHost = mBufferManager->copyFrom(*mLogitsDevice, tensorrt_llm::runtime::MemoryType::kCPU);
            mBufferManager->copy(
                mDecodeLayer->getRuntimeLogitsDevice(), *mRuntimeLogitsHost, tensorrt_llm::runtime::MemoryType::kGPU);
            mStream->synchronize();

            if (greedySearch && !mUseMedusa)
            {
                fillRefLogits(bufferCast<SizeType32>(*seqLenHost), expectedOutputIds, step);
            }

            {
                auto const passed = checkResult(bufferCast<TokenIdType>(*newTokensHost), expectedOutputIds,
                    bufferCast<SizeType32>(*seqLenHost), mMaxTokensPerStep, mBatchBeam, step, /* transposed */ true,
                    /* stride transposed */ mMaxBatchSize * mBeamWidth);
                EXPECT_TRUE(passed) << "New tokens check failed at seed " << seed;
                if (!passed)
                {
                    std::stringstream ss;
                    ss << "New tokens ids:" << std::endl << *newTokensHost;
                    TLLM_LOG_DEBUG(ss.str());
                }
            }

            // Check if logits were not modified in-place
            {
                auto const passed = compareValues(bufferCast<T>(*mLogitsRefHost), bufferCast<T>(*logitsHost),
                    mBatchSize * mMaxTokensPerStep * mBeamWidth * mVocabSizePadded);
                EXPECT_TRUE(passed) << "Unmodified logits check failed at seed " << seed;
            }
        }

        auto const outputIdsHost = mBufferManager->copyFrom(*mOutputIdsDevice, tensorrt_llm::runtime::MemoryType::kCPU);
        auto const seqLenHost = mBufferManager->copyFrom(*mSeqLengthsDevice, tensorrt_llm::runtime::MemoryType::kCPU);
        auto const logProbsHost
            = mBufferManager->copyFrom(*mOutputLogProbsDevice, tensorrt_llm::runtime::MemoryType::kCPU);

        mStream->synchronize();

        {
            auto const passed = checkResult(bufferCast<TokenIdType>(*outputIdsHost), expectedOutputIds,
                bufferCast<SizeType32>(*seqLenHost), mMaxSeqLen, mBatchBeam, /* step */ 0);
            EXPECT_TRUE(passed) << "Output Ids check failed at seed " << seed;
            if (!passed)
            {
                std::stringstream ss;
                ss << "Actual output ids:" << std::endl << *outputIdsHost;
                TLLM_LOG_DEBUG(ss.str());
            }
        }

        if (greedySearch && !mUseMedusa)
        {
            auto const passed = compareValues(
                bufferCast<float>(*logProbsHost), bufferCast<float>(*mRefLogProbsHost), mMaxSeqLen * mMaxBatchSize);
            EXPECT_TRUE(passed) << "Log probs check failed at seed " << seed;
        }
    }

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void DynamicDecodeLayerTest<T>::runTest(
    std::vector<std::set<TokenIdType>> const& expectedOutputIds, TestSamplingParams const& params, TokenIdType endId)
{
    allocateData(params, endId);

    if (!params.useMedusa)
    {
        TLLM_LOG_DEBUG("Run test with linear logits");
        mUseLogitsVec = false;
        runTestImpl(expectedOutputIds, params, endId);
    }
    TLLM_LOG_DEBUG("Run test with vectorized logits");
    mUseLogitsVec = true;
    runTestImpl(expectedOutputIds, params, endId);
}

template class DynamicDecodeLayerTest<float>;
template class DynamicDecodeLayerTest<half>;

TYPED_TEST_SUITE(DynamicDecodeLayerTest, FloatAndHalfTypes);

TYPED_TEST(DynamicDecodeLayerTest, TopK)
{
    SizeType32 topK = 2;
    float topP = 0.0f;
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = {topP};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, // step 0
        {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, // step 1
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, // step 2
        {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopK1TopP0)
{
    SizeType32 topK = 1;
    float topP = 0.0f;
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = {topP};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, BatchTopK)
{
    std::vector<SizeType32> topKs = {2, 1, 1, 2, 1, 1};
    TestSamplingParams params;
    params.topKs = topKs;
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4}, {4}, {4, 5}, {4}, {4}, // step 0
        {0, 1}, {0}, {0}, {0, 1}, {0}, {0}, // step 1
        {2, 3}, {2}, {2}, {2, 3}, {2}, {2}, // step 2
        {0, 1}, {0}, {0}, {0, 1}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKTopP)
{
    SizeType32 topK = 2;
    float topP = 0.3;
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = {topP};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, BatchTopKTopP)
{
    std::vector<SizeType32> topKs = {2, 2, 1, 2, 2, 1};
    float topP = 0.3;
    TestSamplingParams params;
    params.topKs = topKs;
    params.topPs = {topP};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKBatchTopP)
{
    SizeType32 topK = 2;
    std::vector<float> topPs = {0.5, 0.3, 0.5, 0.5, 0.3, 0.5};
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = topPs;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4}, {4, 5}, {4, 5}, {4}, {4, 5}, // step 0
        {0, 1}, {0}, {0, 1}, {0, 1}, {0}, {0, 1}, // step 1
        {2, 3}, {2}, {2, 3}, {2, 3}, {2}, {2, 3}, // step 2
        {0, 1}, {0}, {0, 1}, {0, 1}, {0}, {0, 1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, BatchTopKBatchTopP)
{
    std::vector<SizeType32> topKs = {2, 2, 0, 2, 2, 1};
    std::vector<float> topPs = {0.0, 0.3, 0.5, 0.0, 0.3, 0.5};
    TestSamplingParams params;
    params.topKs = topKs;
    params.topPs = topPs;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4}, {4, 5}, {4, 5}, {4}, {4}, // step 0
        {0, 1}, {0}, {0, 1}, {0, 1}, {0}, {0}, // step 1
        {2, 3}, {2}, {2, 3}, {2, 3}, {2}, {2}, // step 2
        {0, 1}, {0}, {0, 1}, {0, 1}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsZeroTopK)
{
    SizeType32 topK = 0;
    TestSamplingParams params;
    params.topKs = {topK};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsZeroTopP)
{
    float topP = 0;
    TestSamplingParams params;
    params.topPs = {topP};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsZeroTopKTopP)
{
    SizeType32 topK = 0;
    float topP = 0;
    TestSamplingParams params;
    params.topPs = {topP};
    params.topKs = {topK};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsZeroBatchTopKTopP)
{
    std::vector<SizeType32> topKs = {0, 0, 0, 0, 0, 0};
    float topP = 0;
    TestSamplingParams params;
    params.topPs = {topP};
    params.topKs = topKs;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsZeroTopKBatchTopP)
{
    SizeType32 topK = 0;
    std::vector<float> topPs = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    TestSamplingParams params;
    params.topPs = topPs;
    params.topKs = {topK};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsBatchTopKContainZero)
{
    std::vector<SizeType32> topKs = {2, 1, 0, 0, 2, 1};
    TestSamplingParams params;
    params.topKs = topKs;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4}, {4}, {4}, {4, 5}, {4}, // step 0
        {0, 1}, {0}, {0}, {0}, {0, 1}, {0}, // step 1
        {2, 3}, {2}, {2}, {2}, {2, 3}, {2}, // step 2
        {0, 1}, {0}, {0}, {0}, {0, 1}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsBatchTopKTopPContainZero)
{
    std::vector<SizeType32> topKs = {2, 2, 1, 0, 2, 0};
    float topP = 0.0;
    TestSamplingParams params;
    params.topPs = {topP};
    params.topKs = topKs;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4, 5}, {4}, {4}, {4, 5}, {4}, // step 0
        {0, 1}, {0, 1}, {0}, {0}, {0, 1}, {0}, // step 1
        {2, 3}, {2, 3}, {2}, {2}, {2, 3}, {2}, // step 2
        {0, 1}, {0, 1}, {0}, {0}, {0, 1}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, InvalidArgsBatchTopKBatchTopPContainZero)
{
    std::vector<SizeType32> topKs = {0, 2, 1, 2, 2, 0};
    std::vector<float> topPs = {0.0, 0.3, 0.9, 0.0, 0.3, 0.5};
    TestSamplingParams params;
    params.topPs = topPs;
    params.topKs = topKs;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4, 5}, {4}, {4, 5}, // step 0
        {0}, {0}, {0}, {0, 1}, {0}, {0, 1}, // step 1
        {2}, {2}, {2}, {2, 3}, {2}, {2, 3}, // step 2
        {0}, {0}, {0}, {0, 1}, {0}, {0, 1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPTemperature)
{
    float temperature = 0.01f;
    TestSamplingParams params;
    params.temperatures = {temperature};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPTemperatureBatch)
{
    std::vector<float> temperatures = {0.01f, 1e3f, 1.0f, 1.0f, 0.01f, 1.0f};
    TestSamplingParams params;
    params.temperatures = temperatures;
    params.topPs = {0.5f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        {4}, {4, 5, 6, 7}, {4, 5}, {4, 5}, {4}, {4, 5}, // step 0
        {0}, {0, 1, 2, 3}, {0, 1}, {0, 1}, {0}, {0, 1}, // step 1
        {2}, {2, 3, 4, 5}, {2, 3}, {2, 3}, {2}, {2, 3}, // step 2
        {0}, {0, 1, 2, 3}, {0, 1}, {0, 1}, {0}, {0, 1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPTemperatureMultipleRequests)
{
    this->allocateData(TestSamplingParams{});
    {
        std::vector<float> temperatures = {0.01f, 1e3f, 1.0f, 1.0f, 0.01f, 1.0f};
        TestSamplingParams params;
        params.temperatures = temperatures;
        params.topPs = {0.5f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4, 5, 6, 7}, {4, 5}, {4, 5}, {4}, {4, 5}, // step 0
            {0}, {0, 1, 2, 3}, {0, 1}, {0, 1}, {0}, {0, 1}, // step 1
            {2}, {2, 3, 4, 5}, {2, 3}, {2, 3}, {2}, {2, 3}, // step 2
            {0}, {0, 1, 2, 3}, {0, 1}, {0, 1}, {0}, {0, 1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        TestSamplingParams params;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {0}, {0}, {0}, {0}, {0}, {0}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        float temperature = 1.0f;
        TestSamplingParams params;
        params.temperatures = {temperature};
        params.topPs = {0.5f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, // step 0
            {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, // step 1
            {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, // step 2
            {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionPenalty)
{
    SizeType32 topK = 1;
    float repetitionPenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionPenaltiesBatch)
{
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = repetitionPenalties;
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionPenaltyMultipleRequests)
{
    this->allocateData(TestSamplingParams{});
    {
        float repetitionPenalty = 1e9f;
        TestSamplingParams params;
        params.repetitionPenalties = {repetitionPenalty};
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {1}, {1}, {1}, {1}, {1}, {1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        TestSamplingParams params;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {0}, {0}, {0}, {0}, {0}, {0}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
        TestSamplingParams params;
        params.repetitionPenalties = repetitionPenalties;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {1}, {1}, {0}, {0}, {0}, {1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
}

TYPED_TEST(DynamicDecodeLayerTest, TopPPresencePenalty)
{
    float presencePenalty = 1e9f;
    TestSamplingParams params;
    params.presencePenalties = {presencePenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPPresencePenaltiesBatch)
{
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.presencePenalties = presencePenalties;
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPPresencePenaltyMultipleRequests)
{
    this->allocateData(TestSamplingParams{});
    {
        float presencePenalty = 1e9f;
        TestSamplingParams params;
        params.presencePenalties = {presencePenalty};
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {1}, {1}, {1}, {1}, {1}, {1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        TestSamplingParams params;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {0}, {0}, {0}, {0}, {0}, {0}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
        TestSamplingParams params;
        params.presencePenalties = presencePenalties;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {1}, {1}, {0}, {0}, {0}, {1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
}

TYPED_TEST(DynamicDecodeLayerTest, TopPFrequencyPenalty)
{
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.frequencyPenalties = {frequencyPenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPFrequencyPenaltiesBatch)
{
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.frequencyPenalties = frequencyPenalties;
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPFrequencyPenaltyMultipleRequests)
{
    this->allocateData(TestSamplingParams{});
    {
        float frequencyPenalty = 1e9f;
        TestSamplingParams params;
        params.frequencyPenalties = {frequencyPenalty};
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {1}, {1}, {1}, {1}, {1}, {1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        TestSamplingParams params;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {0}, {0}, {0}, {0}, {0}, {0}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
    {
        std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
        TestSamplingParams params;
        params.frequencyPenalties = frequencyPenalties;
        params.topPs = {0.3f};
        std::vector<std::set<TokenIdType>> expectedOutputIds{
            // batch
            {4}, {4}, {4}, {4}, {4}, {4}, // step 0
            {0}, {0}, {0}, {0}, {0}, {0}, // step 1
            {2}, {2}, {2}, {2}, {2}, {2}, // step 2
            {1}, {1}, {0}, {0}, {0}, {1}  // step 3
        };
        this->runTestImpl(expectedOutputIds, params);
    }
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionPresencePenalty)
{
    float repetitionPenalty = 1e9f;
    float presencePenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.presencePenalties = {presencePenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionPresencePenaltiesBatch)
{
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalties};
    params.presencePenalties = {presencePenalties};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionFrequencyPenalty)
{
    float repetitionPenalty = 1e9f;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.frequencyPenalties = {frequencyPenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPRepetitionFrequencyPenaltiesBatch)
{
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalties};
    params.frequencyPenalties = {frequencyPenalties};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPPresenceFrequencyPenalty)
{
    float presencePenalty = 1e9f;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.presencePenalties = {presencePenalty};
    params.frequencyPenalties = {frequencyPenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPPresenceFrequencyPenaltiesBatch)
{
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.presencePenalties = {presencePenalties};
    params.frequencyPenalties = {frequencyPenalties};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPFullPenalty)
{
    float repetitionPenalty = 1e9f;
    float presencePenalty = 1e9f;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.presencePenalties = {presencePenalty};
    params.frequencyPenalties = {frequencyPenalty};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPFullPenaltiesBatch)
{
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalties};
    params.presencePenalties = {presencePenalties};
    params.frequencyPenalties = {frequencyPenalties};
    params.topPs = {0.3f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPMinLengthBatch)
{
    std::vector<SizeType32> minLengths = {3, 1, 1, 3, 0, 3};
    TestSamplingParams params;
    params.minLengths = minLengths;
    params.topPs = {0.3f};
    TokenIdType const endId = 0;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {1}, {0}, {0}, {1}, {0}, {1}, // step 1
        {2}, {0}, {0}, {2}, {0}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params, endId);
}

TYPED_TEST(DynamicDecodeLayerTest, TopPBias)
{
    TestSamplingParams params;
    params.topPs = {0.5f};
    params.useBias = true;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, // step 0
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, // step 1
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, // step 2
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKTemperature)
{
    SizeType32 topK = 2;
    float temperature = 0.01f;
    TestSamplingParams params;
    params.temperatures = {temperature};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKTemperatureBatch)
{
    SizeType32 topK = 2;
    std::vector<float> temperatures = {0.01f, 1e3f, 1.0f, 0.5f, 0.01f, 1.0f};
    TestSamplingParams params;
    params.temperatures = temperatures;
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        {4}, {4, 5, 6, 7}, {4, 5}, {4, 5}, {4}, {4, 5}, // step 0
        {0}, {0, 1, 2, 3}, {0, 1}, {0, 1}, {0}, {0, 1}, // step 1
        {2}, {2, 3, 4, 5}, {2, 3}, {2, 3}, {2}, {2, 3}, // step 2
        {0}, {0, 1, 2, 3}, {0, 1}, {0, 1}, {0}, {0, 1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKRepetitionPenalty)
{
    SizeType32 topK = 1;
    float repetitionPenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKRepetitionPenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = repetitionPenalties;
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKPresencePenalty)
{
    SizeType32 topK = 1;
    float presencePenalty = 1e9f;
    TestSamplingParams params;
    params.presencePenalties = {presencePenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKPresencePenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.presencePenalties = presencePenalties;
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKFrequencyPenalty)
{
    SizeType32 topK = 1;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.frequencyPenalties = {frequencyPenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKFrequencyPenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.frequencyPenalties = frequencyPenalties;
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKRepetitionPresencePenalty)
{
    SizeType32 topK = 1;
    float repetitionPenalty = 1e9f;
    float presencePenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.presencePenalties = {presencePenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKRepetitionPresencePenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalties};
    params.presencePenalties = {presencePenalties};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKRepetitionFrequencyPenalty)
{
    SizeType32 topK = 1;
    float repetitionPenalty = 1e9f;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.frequencyPenalties = {frequencyPenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKRepetitionFrequencyPenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalties};
    params.frequencyPenalties = {frequencyPenalties};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKPresenceFrequencyPenalty)
{
    SizeType32 topK = 1;
    float presencePenalty = 1e9f;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.presencePenalties = {presencePenalty};
    params.frequencyPenalties = {frequencyPenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKPresenceFrequencyPenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.presencePenalties = {presencePenalties};
    params.frequencyPenalties = {frequencyPenalties};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKFullPenalty)
{
    SizeType32 topK = 1;
    float repetitionPenalty = 1e9f;
    float presencePenalty = 1e9f;
    float frequencyPenalty = 1e9f;
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalty};
    params.presencePenalties = {presencePenalty};
    params.frequencyPenalties = {frequencyPenalty};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {1}, {1}, {1}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKFullPenaltiesBatch)
{
    SizeType32 topK = 1;
    std::vector<float> repetitionPenalties = {1e9f, 1e9f, 1.0f, 1.0f, 1.0f, 1e9f};
    std::vector<float> presencePenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    std::vector<float> frequencyPenalties = {1e9f, 1e9f, 0.0f, 0.0f, 0.0f, 1e9f};
    TestSamplingParams params;
    params.repetitionPenalties = {repetitionPenalties};
    params.presencePenalties = {presencePenalties};
    params.frequencyPenalties = {frequencyPenalties};
    params.topKs = {topK};
    params.topPs = {1.0f};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {2}, {2}, {2}, {2}, {2}, {2}, // step 2
        {1}, {1}, {0}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKMinLengthBatch)
{
    SizeType32 topK = 1;
    std::vector<SizeType32> minLengths = {3, 1, 1, 3, 0, 3};
    TestSamplingParams params;
    params.minLengths = minLengths;
    params.topKs = {topK};
    params.topPs = {1.0f};
    TokenIdType const endId = 0;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {1}, {0}, {0}, {1}, {0}, {1}, // step 1
        {2}, {0}, {0}, {2}, {0}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params, endId);
}

TYPED_TEST(DynamicDecodeLayerTest, TopKBias)
{
    SizeType32 topK = 2;
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = {1.0f};
    params.useBias = true;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, {4, 5}, // step 0
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, // step 1
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, // step 2
        {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, BadWords)
{
    SizeType32 topK = 1;
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = {1.0f};
    params.badWords = {{{4, 0}, {2}}, {{0, 2}}, {{4, 0, 2}, {4, 0, 3, 0}}, {{3}}, {{4}, {5}}, {{0}, {3}}};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {6}, {4}, // step 0
        {1}, {0}, {0}, {0}, {0}, {1}, // step 1
        {3}, {3}, {3}, {2}, {2}, {2}, // step 2
        {0}, {0}, {1}, {0}, {0}, {1}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, StopWords)
{
    SizeType32 topK = 1;
    TestSamplingParams params;
    params.topKs = {topK};
    params.topPs = {1.0f};
    params.stopWords = {{{4, 0}, {2}}, {{0, 2}}, {{4, 0, 2}}, {{3}}, {{4}, {5}}, {{4, 0, 2, 0}}};
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {0}, {0}, {0}, // step 1
        {0}, {2}, {2}, {2}, {0}, {2}, // step 2
        {0}, {0}, {0}, {0}, {0}, {0}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

TYPED_TEST(DynamicDecodeLayerTest, MedusaSimpleTest)
{
    TestSamplingParams params;
    params.topKs = {1, 1, 1, 1, 1, 1};
    params.topKMedusaHeads = {{3, 1}, {1, 3}, {3, 1}, {2, 2}, {2, 2}, {1, 3}};
    params.tokensPerStep = {4, 4, 4, 4, 4, 4};
    params.maxNumMedusaHeads = 2;
    // clang-format off
    params.paths = {{0, 1, 2,
                     0, 3, -1},
                    {0, 1, -1,
                     0, -1, -1},
                    {0, 1, 3},
                    {0, 2, 3},
                    {0, 2, -1},
                    {0, 3, -1}};
    // clang-format on
    params.outputIds = {{4, 0, 2}, {4, 0, 2}, {4, 0, 0}, {4, 4, 2}, {4, 0, 2}, {4, 0, 2}};
    params.useMedusa = true;
    std::vector<std::set<TokenIdType>> expectedOutputIds{
        // batch
        {4}, {4}, {4}, {4}, {4}, {4}, // step 0
        {0}, {0}, {0}, {2}, {4}, {4}, // step 1
        {2}, {0}, {0}, {0}, {0}, {0}, // step 2
        {2}, {2}, {0}, {2}, {2}, {2}  // step 3
    };
    this->runTest(expectedOutputIds, params);
}

} // namespace tensorrt_llm::tests::layers::sampling
