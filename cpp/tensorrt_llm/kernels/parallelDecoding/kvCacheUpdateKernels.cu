/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kvCacheUpdateKernels.h"

#include "tensorrt_llm/common/cudaUtils.h"
#include "tensorrt_llm/kernels/kvCacheUtils.h"

#include <array>
#include <vector>

namespace tensorrt_llm::kernels::parallel_decoding
{

static constexpr int kUpdateKVCacheKernelShmSize = 16384;

template <typename KVCacheBuffer, int MaxLayerCount, typename MoveEltType>
__global__ void updateKVCacheDraftTokenLocationBatchedKernel(std::array<KVCacheBuffer, MaxLayerCount> kvCacheBuffers,
    int const* seqAcceptedDraftTokenOffsets, IndexType const* packedAcceptedDraftTokensIndices,
    int32_t const* pastKeyValueLengths, int rewindDraftTokenCommonCount, int const* rewindDraftTokenSeparateAdjustments,
    int const* seqSlotRemapping, int eltCountPerHead)
{
    int seqIdx = blockIdx.x;
    int headIdx = blockIdx.y;
    int layerIdx = blockIdx.z;
    int warpIdx = threadIdx.x / 32;
    int warpCount = blockDim.x / 32;
    int laneIdx = threadIdx.x & 0x1f;
    int seqDraftTokenStart = seqAcceptedDraftTokenOffsets[seqIdx];
    int seqDraftTokenEnd = seqAcceptedDraftTokenOffsets[seqIdx + 1];
    auto const seqSlot = seqSlotRemapping == nullptr ? seqIdx : seqSlotRemapping[seqIdx];
    int seqDraftCount = seqDraftTokenEnd - seqDraftTokenStart;
    int maxEltCountPerMove = kUpdateKVCacheKernelShmSize / sizeof(MoveEltType) / seqDraftCount;
    int eltCountPerMove = min(maxEltCountPerMove, eltCountPerHead);
    if (seqDraftCount == 0 || eltCountPerMove == 0)
    {
        return;
    }
    KVCacheBuffer& kvCacheBuffer = kvCacheBuffers[layerIdx];
    int tokenStartIdx = pastKeyValueLengths[seqSlot] - rewindDraftTokenCommonCount;
    if (rewindDraftTokenSeparateAdjustments != nullptr)
    {
        tokenStartIdx -= rewindDraftTokenSeparateAdjustments[seqSlot];
    }
    __shared__ char loadSmemBuffer[kUpdateKVCacheKernelShmSize];
    auto* eltLoadSmemBuffer = reinterpret_cast<MoveEltType*>(&loadSmemBuffer[0]);
    for (int startChannelOffset = 0; startChannelOffset < eltCountPerHead; startChannelOffset += eltCountPerMove)
    {
        int eltCountCurrentMove = min(eltCountPerMove, eltCountPerHead - startChannelOffset);
        // load K
        for (int tokenIdx = warpIdx; tokenIdx < seqDraftCount; tokenIdx += warpCount)
        {
            int tokenPos = packedAcceptedDraftTokensIndices[seqDraftTokenStart + tokenIdx];
            auto* tokenSmemBuffer = eltLoadSmemBuffer + tokenIdx * eltCountCurrentMove;
            int tokenKVPosition = tokenStartIdx + tokenPos;
            auto* kPtr = reinterpret_cast<MoveEltType*>(kvCacheBuffer.getKBlockPtr(seqSlot, tokenKVPosition));
            for (int loadChannelIdx = laneIdx; loadChannelIdx < eltCountCurrentMove; loadChannelIdx += 32)
            {
                int channelIdx = loadChannelIdx + startChannelOffset;
                int kvLocationIdx = kvCacheBuffer.getKVLocalIdx(tokenKVPosition, headIdx, eltCountPerHead, channelIdx);
                tokenSmemBuffer[loadChannelIdx] = kPtr[kvLocationIdx];
            }
        }
        __syncthreads();
        // store K
        for (int tokenIdx = warpIdx; tokenIdx < seqDraftCount; tokenIdx += warpCount)
        {
            int tokenPos = tokenIdx;
            auto* tokenSmemBuffer = eltLoadSmemBuffer + tokenIdx * eltCountCurrentMove;
            int tokenKVPosition = tokenStartIdx + tokenPos;
            auto* kPtr = reinterpret_cast<MoveEltType*>(kvCacheBuffer.getKBlockPtr(seqSlot, tokenKVPosition));
            for (int loadChannelIdx = laneIdx; loadChannelIdx < eltCountCurrentMove; loadChannelIdx += 32)
            {
                int channelIdx = loadChannelIdx + startChannelOffset;
                int kvLocationIdx = kvCacheBuffer.getKVLocalIdx(tokenKVPosition, headIdx, eltCountPerHead, channelIdx);
                kPtr[kvLocationIdx] = tokenSmemBuffer[loadChannelIdx];
            }
        }
        __syncthreads();
        // load V
        for (int tokenIdx = warpIdx; tokenIdx < seqDraftCount; tokenIdx += warpCount)
        {
            int tokenPos = packedAcceptedDraftTokensIndices[seqDraftTokenStart + tokenIdx];
            auto* tokenSmemBuffer = eltLoadSmemBuffer + tokenIdx * eltCountCurrentMove;
            int tokenKVPosition = tokenStartIdx + tokenPos;
            auto* vPtr = reinterpret_cast<MoveEltType*>(kvCacheBuffer.getVBlockPtr(seqSlot, tokenKVPosition));
            for (int loadChannelIdx = laneIdx; loadChannelIdx < eltCountCurrentMove; loadChannelIdx += 32)
            {
                int channelIdx = loadChannelIdx + startChannelOffset;
                int kvLocationIdx = kvCacheBuffer.getKVLocalIdx(tokenKVPosition, headIdx, eltCountPerHead, channelIdx);
                tokenSmemBuffer[loadChannelIdx] = vPtr[kvLocationIdx];
            }
        }
        __syncthreads();
        // store V
        for (int tokenIdx = warpIdx; tokenIdx < seqDraftCount; tokenIdx += warpCount)
        {
            int tokenPos = tokenIdx;
            auto* tokenSmemBuffer = eltLoadSmemBuffer + tokenPos * eltCountCurrentMove;
            int tokenKVPosition = tokenStartIdx + tokenPos;
            auto* vPtr = reinterpret_cast<MoveEltType*>(kvCacheBuffer.getVBlockPtr(seqSlot, tokenKVPosition));
            for (int loadChannelIdx = laneIdx; loadChannelIdx < eltCountCurrentMove; loadChannelIdx += 32)
            {
                int channelIdx = loadChannelIdx + startChannelOffset;
                int kvLocationIdx = kvCacheBuffer.getKVLocalIdx(tokenKVPosition, headIdx, eltCountPerHead, channelIdx);
                vPtr[kvLocationIdx] = tokenSmemBuffer[loadChannelIdx];
            }
        }
        __syncthreads();
    }
}

template <typename KVCacheBuffer, int MaxLayerCount>
void updateKVCacheDraftTokenLocationBatched(KVCacheBuffer const* kvCacheBuffers,
    int const* seqAcceptedDraftTokenOffsets, IndexType const* packedAcceptedDraftTokensIndices,
    int32_t const* pastKeyValueLengths, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int rewindDraftTokenCommonCount, int* rewindDraftTokenSeparateAdjustments, int const* seqSlotRemapping,
    cudaStream_t stream)
{
    // make sure launch buffer is enough
    static_assert(MaxLayerCount * sizeof(KVCacheBuffer) <= 3072);
    if (seqCount == 0 || layerCount == 0)
    {
        return;
    }
    int alignedBytes = 16;
    while (alignedBytes > 0 && (sizeInBytesPerKVHead % alignedBytes != 0))
    {
        alignedBytes >>= 1;
    }
    TLLM_CHECK_WITH_INFO(alignedBytes > 0, "alignedByte should be positive");
    int eltCountPerHead = sizeInBytesPerKVHead / alignedBytes;
    dim3 grid(seqCount, numKVHeads, layerCount);
    dim3 block(128, 1, 1);
    std::array<KVCacheBuffer, MaxLayerCount> kvCacheBufferArray;
    for (int i = 0; i < layerCount; i++)
    {
        kvCacheBufferArray[i] = kvCacheBuffers[i];
    }
    void (*pKernelFunc)(std::array<KVCacheBuffer, MaxLayerCount>, int const*, IndexType const*, int32_t const*, int,
        int const*, int const*, int)
        = nullptr;
    switch (alignedBytes)
    {
    case 16:
    {
        pKernelFunc = &updateKVCacheDraftTokenLocationBatchedKernel<KVCacheBuffer, MaxLayerCount, int4>;
        break;
    }
    case 8:
    {
        pKernelFunc = &updateKVCacheDraftTokenLocationBatchedKernel<KVCacheBuffer, MaxLayerCount, int64_t>;
        break;
    }
    case 4:
    {
        pKernelFunc = &updateKVCacheDraftTokenLocationBatchedKernel<KVCacheBuffer, MaxLayerCount, int32_t>;
        break;
    }
    case 2:
    {
        pKernelFunc = &updateKVCacheDraftTokenLocationBatchedKernel<KVCacheBuffer, MaxLayerCount, int16_t>;
        break;
    }
    default:
    {
        TLLM_CHECK_WITH_INFO(alignedBytes == 1, "Strange alignedBytes");
        pKernelFunc = &updateKVCacheDraftTokenLocationBatchedKernel<KVCacheBuffer, MaxLayerCount, int8_t>;
        break;
    }
    }
    pKernelFunc<<<grid, block, 0, stream>>>(kvCacheBufferArray, seqAcceptedDraftTokenOffsets,
        packedAcceptedDraftTokensIndices, pastKeyValueLengths, rewindDraftTokenCommonCount,
        rewindDraftTokenSeparateAdjustments, seqSlotRemapping, eltCountPerHead);
    TLLM_CUDA_CHECK(cudaGetLastError());
}

/*!
 * Update KV cache for parallel decoding algorithms.
 * In following examples, we assume we have 2 sequences, accepted count is [3, 2]
 * @tparam KVCacheBuffer : Type of KV cache, should be LinearKVCache or KVBlockArray
 * @param kvCacheBuffers : list of KVCacheBuffer object
 * @param seqAcceptedDraftTokenOffsets : Array of length seqCount + 1, like [0, 3, 5]
 * @param packedAcceptedDraftTokensIndices : Array of length seqAcceptedDraftTokenOffsets[seqCount], each value is in
 * range [0, maxDraftTokenCount - 1]
 * @param pastKeyValueLengths : Array of length seqCount, meaning how many tokens are already in KV cache
 * @param seqCount : Count of sequence
 * @param numKVHeads : Number of KV heads
 * @param sizeInBytesPerKVHead : Size of each KV head
 * @param rewindDraftTokenCommonCount : Common count to rewind
 * @param rewindDraftTokenSeparateAdjustments : Separate adjustment to rewind for each sequence, if nullptr, just use
 * rewindDraftTokenCommonCount, else use rewindDraftTokenSeparateAdjustments[i] + rewindDraftTokenCommonCount
 * @param stream : CUDA stream to use.
 */
template <typename KVCacheBuffer>
void updateKVCacheDraftTokenLocation(KVCacheBuffer const* kvCacheBuffers, int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths, int layerCount, int seqCount,
    int numKVHeads, int sizeInBytesPerKVHead, int rewindDraftTokenCommonCount, int* rewindDraftTokenSeparateAdjustments,
    int const* seqSlotRemapping, cudaStream_t stream)
{
    int startLayer = 0;
    static constexpr int kMaxLayersPerIter = 32;
    while (startLayer < layerCount)
    {
        int microBatchLayerCount = std::min(layerCount - startLayer, kMaxLayersPerIter);
        updateKVCacheDraftTokenLocationBatched<KVCacheBuffer, kMaxLayersPerIter>(kvCacheBuffers + startLayer,
            seqAcceptedDraftTokenOffsets, packedAcceptedDraftTokensIndices, pastKeyValueLengths, microBatchLayerCount,
            seqCount, numKVHeads, sizeInBytesPerKVHead, rewindDraftTokenCommonCount,
            rewindDraftTokenSeparateAdjustments, seqSlotRemapping, stream);
        startLayer += microBatchLayerCount;
    }
}

void updateLinearKVCacheDraftTokenLocation(int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths,
    int8_t* const* pastKeyValueList, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int rewindDraftTokenCommonCount, int* rewindDraftTokenSeparateAdjustments, int const* seqSlotRemapping,
    int maxKVCacheLen, cudaStream_t stream)
{
    std::vector<KVLinearBuffer> kvLinearBuffers;
    kvLinearBuffers.reserve(layerCount);
    auto const sizePerToken = numKVHeads * sizeInBytesPerKVHead;
    for (int i = 0; i < layerCount; i++)
    {
        kvLinearBuffers.emplace_back(
            seqCount, maxKVCacheLen, sizePerToken, maxKVCacheLen, 0, false, pastKeyValueList[i]);
    }
    updateKVCacheDraftTokenLocation(kvLinearBuffers.data(), seqAcceptedDraftTokenOffsets,
        packedAcceptedDraftTokensIndices, pastKeyValueLengths, layerCount, seqCount, numKVHeads, sizeInBytesPerKVHead,
        rewindDraftTokenCommonCount, rewindDraftTokenSeparateAdjustments, seqSlotRemapping, stream);
}

void updateKVBlockArrayDraftTokenLocation(int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths, void* const* pointerArray,
    KVBlockArray::DataType* offsetArray, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int rewindDraftTokenCommonCount, int* rewindDraftTokenSeparateAdjustments, int const* seqSlotRemapping,
    int maxKVCacheLen, int maxBlocksPerSeq, int tokensPerBlock, cudaStream_t stream)
{
    std::vector<KVBlockArray> kvBlockArrays;
    kvBlockArrays.reserve(layerCount);
    auto const bytesPerToken = numKVHeads * sizeInBytesPerKVHead;
    auto const bytesPerBlock = tokensPerBlock * bytesPerToken;
    for (int layerIdx = 0; layerIdx < layerCount; layerIdx++)
    {
        auto const layerOffset = layerIdx * 2 * bytesPerBlock;
        auto* const primaryPoolPointer
            = reinterpret_cast<void*>(reinterpret_cast<char*>(pointerArray[0]) + layerOffset);
        auto* const secondaryPoolPointer
            = reinterpret_cast<void*>(reinterpret_cast<char*>(pointerArray[1]) + layerOffset);

        kvBlockArrays.emplace_back(seqCount, maxBlocksPerSeq, tokensPerBlock, bytesPerToken, maxKVCacheLen, 0,
            primaryPoolPointer, secondaryPoolPointer, offsetArray);
    }
    updateKVCacheDraftTokenLocation(kvBlockArrays.data(), seqAcceptedDraftTokenOffsets,
        packedAcceptedDraftTokensIndices, pastKeyValueLengths, layerCount, seqCount, numKVHeads, sizeInBytesPerKVHead,
        rewindDraftTokenCommonCount, rewindDraftTokenSeparateAdjustments, seqSlotRemapping, stream);
}

void updateLinearKVCacheDraftTokenLocationCommonRewind(int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths,
    int8_t* const* pastKeyValueList, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int rewindDraftTokenCount, int const* seqSlotRemapping, int maxKVCacheLen, cudaStream_t stream)
{
    updateLinearKVCacheDraftTokenLocation(seqAcceptedDraftTokenOffsets, packedAcceptedDraftTokensIndices,
        pastKeyValueLengths, pastKeyValueList, layerCount, seqCount, numKVHeads, sizeInBytesPerKVHead,
        rewindDraftTokenCount, nullptr, seqSlotRemapping, maxKVCacheLen, stream);
}

void updateKVBlockArrayDraftTokenLocationCommonRewind(int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths, void* const* pointerArray,
    KVBlockArray::DataType* offsetArray, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int rewindDraftTokenCount, int const* seqSlotRemapping, int maxKVCacheLen, int maxBlocksPerSeq, int tokensPerBlock,
    cudaStream_t stream)
{
    updateKVBlockArrayDraftTokenLocation(seqAcceptedDraftTokenOffsets, packedAcceptedDraftTokensIndices,
        pastKeyValueLengths, pointerArray, offsetArray, layerCount, seqCount, numKVHeads, sizeInBytesPerKVHead,
        rewindDraftTokenCount, nullptr, seqSlotRemapping, maxKVCacheLen, maxBlocksPerSeq, tokensPerBlock, stream);
}

void updateLinearKVCacheDraftTokenLocationSeparateRewind(int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths,
    int8_t* const* pastKeyValueList, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int* rewindDraftTokenCounts, int const* seqSlotRemapping, int maxKVCacheLen, cudaStream_t stream)
{
    updateLinearKVCacheDraftTokenLocation(seqAcceptedDraftTokenOffsets, packedAcceptedDraftTokensIndices,
        pastKeyValueLengths, pastKeyValueList, layerCount, seqCount, numKVHeads, sizeInBytesPerKVHead, 0,
        rewindDraftTokenCounts, seqSlotRemapping, maxKVCacheLen, stream);
}

void updateKVBlockArrayDraftTokenLocationSeparateRewind(int const* seqAcceptedDraftTokenOffsets,
    IndexType const* packedAcceptedDraftTokensIndices, int32_t const* pastKeyValueLengths, void* const* pointerArray,
    KVBlockArray::DataType* offsetArray, int layerCount, int seqCount, int numKVHeads, int sizeInBytesPerKVHead,
    int* rewindDraftTokenCounts, int const* seqSlotRemapping, int maxKVCacheLen, int maxBlocksPerSeq,
    int tokensPerBlock, cudaStream_t stream)
{
    updateKVBlockArrayDraftTokenLocation(seqAcceptedDraftTokenOffsets, packedAcceptedDraftTokensIndices,
        pastKeyValueLengths, pointerArray, offsetArray, layerCount, seqCount, numKVHeads, sizeInBytesPerKVHead, 0,
        rewindDraftTokenCounts, seqSlotRemapping, maxKVCacheLen, maxBlocksPerSeq, tokensPerBlock, stream);
}

} // namespace tensorrt_llm::kernels::parallel_decoding
