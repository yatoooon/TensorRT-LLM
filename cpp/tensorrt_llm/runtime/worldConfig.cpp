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

#include "tensorrt_llm/runtime/worldConfig.h"

#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/common/mpiUtils.h"
#include "tensorrt_llm/common/stringUtils.h"

#include <algorithm>
#include <numeric>
#include <set>

using namespace tensorrt_llm::runtime;
namespace tc = tensorrt_llm::common;

WorldConfig::WorldConfig(SizeType32 tensorParallelism, SizeType32 pipelineParallelism, SizeType32 rank,
    SizeType32 gpusPerNode, std::optional<std::vector<SizeType32>> const& deviceIds)
    : mTensorParallelism{tensorParallelism}
    , mPipelineParallelism{pipelineParallelism}
    , mRank{rank}
    , mGpusPerNode{gpusPerNode}
    , mDeviceIds{deviceIds.value_or(std::vector<SizeType32>(mGpusPerNode))}
{
#if ENABLE_MULTI_DEVICE
    auto const numDevices = mDeviceIds.size();
    TLLM_CHECK(numDevices > 0);

    if (!deviceIds.has_value())
    {
        mDeviceIds.resize(mGpusPerNode);
        std::iota(mDeviceIds.begin(), mDeviceIds.end(), 0);
    }
    else
    {
        // total number is at most mGpusPerNode
        TLLM_CHECK_WITH_INFO(static_cast<SizeType32>(numDevices) <= mGpusPerNode,
            "Number of device IDs %zu is greater than GPUs per node %d", numDevices, mGpusPerNode);

        // all deviceIds is within the range
        TLLM_CHECK(*std::max_element(mDeviceIds.begin(), mDeviceIds.end()) < mGpusPerNode);
        TLLM_CHECK(*std::min_element(mDeviceIds.begin(), mDeviceIds.end()) >= 0);

        // all ids are unique
        std::set<SizeType32> const deviceIdSet(mDeviceIds.begin(), mDeviceIds.end());
        TLLM_CHECK_WITH_INFO(
            deviceIdSet.size() == numDevices, "Device IDs are not unique %zu != %zu", deviceIdSet.size(), numDevices);

        // log a warning if device ids are not contiguous
        if (std::adjacent_find(deviceIdSet.begin(), deviceIdSet.end(), [](auto x, auto y) { return y - x != 1; })
            != deviceIdSet.end())
        {
            TLLM_LOG_WARNING("The user specified device IDs are not contiguous!");
        }
        TLLM_LOG_INFO("Using user-specified devices: %s", tc::arr2str(mDeviceIds.data(), numDevices).c_str());
    }

    TLLM_CHECK(mTensorParallelism > 0);
    TLLM_CHECK(mPipelineParallelism > 0);
#else
    // Overriding to default - single GPU
    mRank = 0;
    mGpusPerNode = 1;
    mTensorParallelism = 1;
    mPipelineParallelism = 1;
#endif
}

bool WorldConfig::validMpiConfig() const
{
    return COMM_SESSION.getSize() == getSize();
}

WorldConfig WorldConfig::mpi(SizeType32 gpusPerNode, std::optional<SizeType32> tensorParallelism,
    std::optional<SizeType32> pipelineParallelism, std::optional<std::vector<SizeType32>> const& deviceIds)
{
#if ENABLE_MULTI_DEVICE
    auto& comm = COMM_SESSION;
    auto const mpiSize = comm.getSize();
    auto const mpiRank = comm.getRank();
    TLLM_LOG_INFO("MPI size: %d, rank: %d", mpiSize, mpiRank);
    auto const pp = pipelineParallelism.value_or(1);
    auto const tp = tensorParallelism.value_or(mpiSize / pp);
    TLLM_LOG_DEBUG("TP: %d, PP: %d", tp, pp);
    TLLM_CHECK(mpiSize == tp * pp);
    TLLM_CHECK(mpiSize <= gpusPerNode || LOCAL_COMM_SESSION.getSize() == gpusPerNode);

    return WorldConfig{tp, pp, mpiRank, gpusPerNode, deviceIds};
#else
    return WorldConfig();
#endif
}

std::vector<SizeType32> WorldConfig::getPipelineParallelGroup() const
{
    auto const pp = getPipelineParallelism();
    auto const tp = getTensorParallelism();
    auto const worldSize = getSize();
    std::vector<SizeType32> group;
    group.reserve(pp);
    for (SizeType32 idx = getTensorParallelRank(); idx < worldSize; idx += tp)
    {
        group.push_back(idx);
    }
    return group;
}

std::vector<SizeType32> WorldConfig::getTensorParallelGroup() const
{
    auto const tp = getTensorParallelism();
    auto const rank = getRank();
    auto const tpRank = getTensorParallelRank();
    std::vector<SizeType32> group;
    group.reserve(tp);
    for (SizeType32 idx = 0; idx < tp; idx++)
    {
        group.push_back(rank - tpRank + idx);
    }
    return group;
}
