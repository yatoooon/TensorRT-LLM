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
#ifndef TOP_LEVEL_DIR
#error "Define TOP_LEVEL_DIR"
#endif

#include <gtest/gtest.h>

#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/kernels/decodingCommon.h"
#include "tensorrt_llm/kernels/stopCriteriaKernels.h"
#include "tensorrt_llm/runtime/bufferManager.h"
#include <algorithm>
#include <curand_kernel.h>
#include <random>

namespace tk = tensorrt_llm::kernels;
namespace tc = tensorrt_llm::common;

using namespace tensorrt_llm::runtime;

namespace
{

class StopCriteriaKernelsTest : public testing::Test
{
public:
    using TensorPtr = tensorrt_llm::runtime::ITensor::SharedPtr;

    void SetUp() override
    {
        mStream = std::make_shared<tensorrt_llm::runtime::CudaStream>();
        mBufferManager = std::make_shared<tensorrt_llm::runtime::BufferManager>(mStream);
    }

    void TearDown() override {}

    void initData(SizeType32 seed, std::vector<std::vector<std::vector<SizeType32>>> const& stopWords,
        SizeType32 maxStopWordsLen, SizeType32 batchSize, SizeType32 beamWidth)
    {
        auto const maxBatchSize = 2 * batchSize;

        std::mt19937 generator(seed);
        std::uniform_int_distribution<int> seqLenDistr(0, mMaxSeqLen);

        mSequenceLengths
            = BufferManager::pinned(ITensor::makeShape({maxBatchSize, beamWidth}), nvinfer1::DataType::kINT32);
        mSequenceLengthLimits = BufferManager::pinned(ITensor::makeShape({maxBatchSize}), nvinfer1::DataType::kINT32);
        mFinished = BufferManager::pinned(
            ITensor::makeShape({maxBatchSize, beamWidth}), TRTDataType<tk::FinishedState::UnderlyingType>::value);
        mFinishedSum = BufferManager::pinned(ITensor::makeShape({maxBatchSize}), nvinfer1::DataType::kINT32);

        mOutputIds = BufferManager::pinned(
            ITensor::makeShape({maxBatchSize, beamWidth, mMaxSeqLen}), nvinfer1::DataType::kINT32);
        mOutputIdsPtr
            = BufferManager::pinned(ITensor::makeShape({maxBatchSize, beamWidth}), nvinfer1::DataType::kINT64);

        mParentIds = BufferManager::pinned(
            ITensor::makeShape({maxBatchSize, beamWidth, mMaxSeqLen}), nvinfer1::DataType::kINT32);
        mParentIdsPtr
            = BufferManager::pinned(ITensor::makeShape({maxBatchSize, beamWidth}), nvinfer1::DataType::kINT64);

        mRefOutputIds = BufferManager::pinned(
            ITensor::makeShape({maxBatchSize, beamWidth, mMaxSeqLen}), nvinfer1::DataType::kINT32);

        mStopWords
            = BufferManager::pinned(ITensor::makeShape({maxBatchSize, 2, maxStopWordsLen}), nvinfer1::DataType::kINT32);
        mStopWordsPtr = BufferManager::pinned(ITensor::makeShape({maxBatchSize}), nvinfer1::DataType::kINT64);
        mStopWordsLen = BufferManager::pinned(ITensor::makeShape({maxBatchSize}), nvinfer1::DataType::kINT32);

        mBatchSlots = BufferManager::pinned(ITensor::makeShape({batchSize}), nvinfer1::DataType::kINT32);

        auto batchSlotsPtr = bufferCast<int32_t>(*mBatchSlots);
        for (SizeType32 bi = 0; bi < batchSize; ++bi)
        {
            batchSlotsPtr[bi] = 2 * bi;
        }

        auto sequenceLengthsPtr = bufferCast<SizeType32>(*mSequenceLengths);
        auto sequenceLengthLimitsPtr = bufferCast<SizeType32>(*mSequenceLengthLimits);
        auto finishedPtr
            = reinterpret_cast<tk::FinishedState*>(bufferCast<tk::FinishedState::UnderlyingType>(*mFinished));
        auto finishedSumPtr = bufferCast<SizeType32>(*mFinishedSum);

        for (SizeType32 bi = 0; bi < maxBatchSize; ++bi)
        {
            for (SizeType32 ri = 0; ri < beamWidth; ri++)
            {
                sequenceLengthsPtr[bi * beamWidth + ri] = maxStopWordsLen == 0
                    ? seqLenDistr(generator)
                    : mMaxSeqLen - (static_cast<SizeType32>(bi / 2) + ri) % mMaxSeqLen;
                finishedPtr[bi * beamWidth + ri] = tk::FinishedState::empty();
            }
        }
        for (SizeType32 bi = 0; bi < maxBatchSize; ++bi)
        {
            sequenceLengthLimitsPtr[bi] = maxStopWordsLen == 0
                ? seqLenDistr(generator)
                : mMaxSeqLen - static_cast<SizeType32>(bi / 2) % mMaxSeqLen;
            finishedSumPtr[bi] = 0;
        }

        auto outputIdsPtrsData = reinterpret_cast<void**>(bufferCast<int64_t>(*mOutputIdsPtr));
        auto parentIdsPtrsData = reinterpret_cast<void**>(bufferCast<int64_t>(*mParentIdsPtr));
        auto outputIdsData = bufferCast<int32_t>(*mOutputIds);
        auto refOutputIdsData = bufferCast<int32_t>(*mRefOutputIds);
        auto parentIdsData = bufferCast<int32_t>(*mParentIds);

        // Tokens ids are
        // bi: 0, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
        // bi: 0, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30]
        // bi: 1, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
        // bi: 1, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30]
        // bi: 2, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
        // bi: 2, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29]
        // bi: 3, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
        // bi: 3, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29]
        // bi: 4, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]
        // bi: 4, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28]
        // bi: 5, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]
        // bi: 5, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28]
        // bi: 6, ri: 0: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]
        // bi: 6, ri: 1: [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27]
        for (SizeType32 bi = 0; bi < maxBatchSize; bi++)
        {
            for (SizeType32 ri = 0; ri < beamWidth; ri++)
            {
                for (SizeType32 si = 0; si < mMaxSeqLen; si++)
                {
                    auto const idx = tc::flat_index3(bi, ri, si, beamWidth, mMaxSeqLen);
                    outputIdsData[idx] = ri * mMaxSeqLen + si;
                    parentIdsData[idx] = 0;
                }
            }
        }

        for (SizeType32 bi = 0; bi < maxBatchSize; bi++)
        {
            outputIdsPtrsData[bi] = outputIdsData + bi * beamWidth * mMaxSeqLen;
            parentIdsPtrsData[bi] = parentIdsData + bi * beamWidth * mMaxSeqLen;
        }

        // Init stop words tensor
        auto stopWordsData = bufferCast<int32_t>(*mStopWords);
        std::fill(stopWordsData, stopWordsData + maxBatchSize * 2 * maxStopWordsLen, -1);
        for (SizeType32 bi = 0; bi < stopWords.size(); bi++)
        {
            SizeType32 totalLen = 0;
            for (SizeType32 wi = 0; wi < stopWords[bi].size(); ++wi)
            {
                for (SizeType32 si = 0; si < stopWords[bi][wi].size(); ++si)
                {
                    stopWordsData[bi * 2 * maxStopWordsLen + 0 * maxStopWordsLen + totalLen + si]
                        = stopWords[bi][wi][si];
                }
                totalLen += stopWords[bi][wi].size();
                // Do not add value if stop words is empty
                if (totalLen > 0)
                {
                    stopWordsData[bi * 2 * maxStopWordsLen + 1 * maxStopWordsLen + wi] = totalLen;
                }
            }
            // Special case when all stop words are of single token length
            if (stopWords[bi].size() == totalLen)
            {
                stopWordsData[bi * 2 * maxStopWordsLen + 1 * maxStopWordsLen + totalLen] = totalLen + 1;
            }
        }

        auto stopWordsPtr = BufferRange<int32_t*>(*mStopWordsPtr);
        auto stopWordsLensPtr = bufferCast<int32_t>(*mStopWordsLen);
        for (SizeType32 bi = 0; bi < stopWords.size(); bi++)
        {
            stopWordsPtr[bi] = stopWordsData + bi * 2 * maxStopWordsLen;

            SizeType32 stopWordsLen = 0;
            for (auto const& words : stopWords[bi])
            {
                stopWordsLen += words.size();
            }
            if (stopWordsLen == stopWords[bi].size())
            {
                stopWordsLen += 1;
            }
            stopWordsLensPtr[bi] = stopWordsLen;
        }
    }

    void verifyMaxSeqLenStopCriteriaResults(SizeType32 seed, SizeType32 batchSize, SizeType32 beamWidth)
    {
        mStream->synchronize();

        auto sequenceLengthsPtr = bufferCast<SizeType32>(*mSequenceLengths);
        auto sequenceLengthLimitsPtr = bufferCast<SizeType32>(*mSequenceLengthLimits);
        auto finishedPtr
            = reinterpret_cast<tk::FinishedState*>(bufferCast<tk::FinishedState::UnderlyingType>(*mFinished));
        auto finishedSumPtr = bufferCast<SizeType32>(*mFinishedSum);
        auto batchSlotsPtr = bufferCast<int32_t>(*mBatchSlots);

        for (SizeType32 batchIdx = 0; batchIdx < batchSize; ++batchIdx)
        {
            int32_t refSumFinished = 0;
            auto const batchSlot = batchSlotsPtr[batchIdx];
            for (SizeType32 beamIdx = 0; beamIdx < beamWidth; ++beamIdx)
            {
                auto const batchBeamIdx = batchSlot * beamWidth + beamIdx;
                auto const limitExceeded = sequenceLengthsPtr[batchBeamIdx] >= sequenceLengthLimitsPtr[batchSlot];
                refSumFinished += limitExceeded;
                if (limitExceeded)
                {
                    EXPECT_TRUE(finishedPtr[batchBeamIdx].isFinishedMaxLength())
                        << " batchIdx: " << batchIdx << " beamIdx: " << beamIdx << " seed: " << seed;
                }
            }
            EXPECT_EQ(refSumFinished, finishedSumPtr[batchSlot]);
        }
    }

    bool isSubsequence(SizeType32 const* sequence, SizeType32 n, std::vector<int> const& subsequence)
    {
        auto it = std::search(sequence, sequence + n, subsequence.begin(), subsequence.end());
        return it != sequence + n;
    }

    void verifyStopWordsStopCriteriaResults(SizeType32 seed,
        std::vector<std::vector<std::vector<SizeType32>>> const& stopWords, SizeType32 stopWordsLen,
        SizeType32 batchSize, SizeType32 beamWidth)
    {
        mStream->synchronize();

        auto outputIdsData = bufferCast<int32_t>(*mOutputIds);
        auto finishedPtr
            = reinterpret_cast<tk::FinishedState*>(bufferCast<tk::FinishedState::UnderlyingType>(*mFinished));
        auto sequenceLengthsPtr = bufferCast<SizeType32>(*mSequenceLengths);
        auto batchSlotsPtr = bufferCast<int32_t>(*mBatchSlots);

        for (SizeType32 bi = 0; bi < batchSize; bi++)
        {
            auto const batchSlot = batchSlotsPtr[bi];
            for (SizeType32 bwi = 0; bwi < beamWidth; bwi++)
            {
                auto outputIdsBatchBeam = outputIdsData + batchSlot * beamWidth * mMaxSeqLen + bwi * mMaxSeqLen;
                bool found = false;
                for (SizeType32 wi = 0; wi < stopWords[batchSlot].size(); ++wi)
                {
                    auto const wordLen = stopWords[batchSlot][wi].size();
                    auto const seqLen = sequenceLengthsPtr[batchSlot * beamWidth + bwi];
                    auto const offset = seqLen - wordLen;
                    found |= isSubsequence(outputIdsBatchBeam + offset, wordLen, stopWords[batchSlot][wi]);
                    if (found)
                    {
                        EXPECT_TRUE(finishedPtr[batchSlot * beamWidth + bwi].isFinishedStopWords());
                        break;
                    }
                }
                if (!found)
                {
                    EXPECT_FALSE(finishedPtr[batchSlot * beamWidth + bwi].isFinished());
                }
            }
        }
    }

    void runStopWordsCriteriaTest(
        std::vector<std::vector<std::vector<SizeType32>>> const& stopWords, SizeType32 batchSize, SizeType32 beamWidth)
    {
        SizeType32 maxStopWordsLen = 0;
        for (auto const& batchStopWords : stopWords)
        {
            SizeType32 stopWordsLen = 0;
            for (auto const& words : batchStopWords)
            {
                stopWordsLen += words.size();
            }
            if (stopWordsLen == batchStopWords.size())
            {
                stopWordsLen += 1;
            }
            maxStopWordsLen = std::max(maxStopWordsLen, stopWordsLen);
        }

        initData(0, stopWords, maxStopWordsLen, batchSize, beamWidth);

        tk::invokeStopWordsCriterion(reinterpret_cast<int32_t const**>(bufferCast<int64_t>(*mOutputIdsPtr)),
            reinterpret_cast<int32_t const**>(bufferCast<int64_t>(*mParentIdsPtr)),
            reinterpret_cast<int32_t const**>(bufferCast<int64_t>(*mStopWordsPtr)),
            reinterpret_cast<tk::FinishedState*>(bufferCast<tk::FinishedState::UnderlyingType>(*mFinished)),
            bufferCast<SizeType32>(*mSequenceLengths), bufferCast<int32_t>(*mBatchSlots),
            bufferCast<SizeType32>(*mStopWordsLen), maxStopWordsLen, batchSize, beamWidth, mMaxSeqLen, mStream->get());

        verifyStopWordsStopCriteriaResults(0, stopWords, maxStopWordsLen, batchSize, beamWidth);
    }

    void runMaxLengthCriteriaTest(SizeType32 seed, SizeType32 batchSize, SizeType32 beamWidth)
    {
        initData(seed, {}, 0, batchSize, beamWidth);

        tk::invokeLengthCriterion(
            reinterpret_cast<tk::FinishedState*>(bufferCast<tk::FinishedState::UnderlyingType>(*mFinished)),
            bufferCast<SizeType32>(*mFinishedSum),
            reinterpret_cast<SizeType32 const*>(bufferCast<SizeType32>(*mSequenceLengthLimits)),
            bufferCast<SizeType32>(*mSequenceLengths), bufferCast<int32_t>(*mBatchSlots), batchSize, beamWidth,
            mStream->get());

        verifyMaxSeqLenStopCriteriaResults(seed, batchSize, beamWidth);
    }

protected:
    std::shared_ptr<tensorrt_llm::runtime::BufferManager> mBufferManager;
    std::shared_ptr<tensorrt_llm::runtime::CudaStream> mStream;

    TensorPtr mSequenceLengths;
    TensorPtr mSequenceLengthLimits;
    TensorPtr mFinished;
    TensorPtr mFinishedSum;

    TensorPtr mOutputIds;
    TensorPtr mRefOutputIds;
    TensorPtr mOutputIdsPtr;
    TensorPtr mParentIds;
    TensorPtr mParentIdsPtr;
    TensorPtr mStopWords;
    TensorPtr mStopWordsPtr;
    TensorPtr mStopWordsLen;
    TensorPtr mBatchSlots;

    static constexpr SizeType32 mMaxSeqLen{16};
    static constexpr SizeType32 mVocabSize{32};
};

TEST_F(StopCriteriaKernelsTest, maxLengthCriteriaBS1BW1Test)
{
    constexpr SizeType32 seeds = 64;
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 1;
    for (SizeType32 seed = 0; seed < seeds; ++seed)
    {
        this->runMaxLengthCriteriaTest(seed, batchSize, beamWidth);
    }
}

TEST_F(StopCriteriaKernelsTest, maxLengthCriteriaBS1BW2Test)
{
    constexpr SizeType32 seeds = 64;
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 2;
    for (SizeType32 seed = 0; seed < seeds; ++seed)
    {
        this->runMaxLengthCriteriaTest(seed, batchSize, beamWidth);
    }
}

TEST_F(StopCriteriaKernelsTest, maxLengthCriteriaBS1024BW1Test)
{
    constexpr SizeType32 seeds = 64;
    constexpr SizeType32 batchSize = 1024;
    constexpr SizeType32 beamWidth = 1;
    for (SizeType32 seed = 0; seed < seeds; ++seed)
    {
        this->runMaxLengthCriteriaTest(seed, batchSize, beamWidth);
    }
}

TEST_F(StopCriteriaKernelsTest, maxLengthCriteriaBS1024BW2Test)
{
    constexpr SizeType32 seeds = 64;
    constexpr SizeType32 batchSize = 1024;
    constexpr SizeType32 beamWidth = 2;
    for (SizeType32 seed = 0; seed < seeds; ++seed)
    {
        this->runMaxLengthCriteriaTest(seed, batchSize, beamWidth);
    }
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS1SingleTokenSingleWordTest)
{
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 1;
    // Expected to not match any word
    this->runStopWordsCriteriaTest({{{2}}, {{2}}}, batchSize, beamWidth);
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS1SingleTokenMultipleWordsTest)
{
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 1;
    // Expected to match 15
    this->runStopWordsCriteriaTest({{{145}, {4}, {1}, {15}}, {{}}}, batchSize, beamWidth);
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS1MultipleTokensSingleWordTest)
{
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 1;
    // Expected to not match any word
    this->runStopWordsCriteriaTest({{{2, 3}}, {{}}}, batchSize, beamWidth);
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS1MultipleTokensMultipleWordsMatchTest)
{
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 1;
    // Expected to match {13, 14, 15}
    this->runStopWordsCriteriaTest({{{1, 4}, {2, 3}, {13, 14, 15}}, {{}}}, batchSize, beamWidth);
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS1MultipleTokensMultipleWordsNotMatchTest)
{
    constexpr SizeType32 batchSize = 1;
    constexpr SizeType32 beamWidth = 1;
    // Expected to not match any word
    this->runStopWordsCriteriaTest({{{1, 4}, {2, 3}, {12, 14, 15}}, {{}}}, batchSize, beamWidth);
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS4MultipleTokensMultipleWordsTest)
{
    constexpr SizeType32 batchSize = 4;
    constexpr SizeType32 beamWidth = 1;
    // Expected to match {12, 13} for the 5th instance in the batch
    this->runStopWordsCriteriaTest(
        {{{2}}, {{}}, {{}}, {{}}, {{15}, {12, 13}}, {{}}, {{1}, {8, 9}}, {{}}}, batchSize, beamWidth);
}

TEST_F(StopCriteriaKernelsTest, stopWordsCriteriaBS4BW2MultipleTokensMultipleWordsTest)
{
    constexpr SizeType32 batchSize = 4;
    constexpr SizeType32 beamWidth = 2;
    // Expected to match {12, 13} to {bi, bw}={{5, 0}}
    // Expected to match {11, 12} to {bi, bw}={{7, 0}}
    // Expected to match {27} to {bi, bw}={{5, 1}}
    this->runStopWordsCriteriaTest(
        {{{2}}, {{}}, {{}}, {{}}, {{11}, {12, 13}}, {{}}, {{27}, {11, 12}}, {{}}}, batchSize, beamWidth);
}

} // end of namespace
