/*
 * Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
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
#include "cubinObj.h"

#include "serializationUtils.h"
#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/cudaDriverWrapper.h"
#include "tensorrt_llm/common/cudaUtils.h"
#include <cuda_runtime_api.h>

namespace tensorrt_llm
{
namespace kernels
{
namespace jit
{

CubinObj::CubinObj(void const* buffer_, size_t buffer_size)
    : mDriver(tensorrt_llm::common::CUDADriverWrapper::getInstance())
{
    uint8_t const* buffer = static_cast<uint8_t const*>(buffer_);
    size_t remaining_buffer_size = buffer_size;
    uint32_t len = readFromBuffer<uint32_t>(buffer, remaining_buffer_size);
    mContent.resize(len);
    TLLM_CHECK(len <= remaining_buffer_size);
    memcpy(mContent.data(), buffer, len);

    initialize(mContent.c_str(), "kernel_mha");
}

size_t CubinObj::getSerializationSize() const noexcept
{
    size_t result = sizeof(uint32_t) + mContent.size();
    // Make result multiples of 4.
    result = (result + 3) & ~3;
    return result;
}

void CubinObj::serialize(void* buffer_, size_t buffer_size) const noexcept
{
    size_t remaining_buffer_size = buffer_size;
    uint8_t* buffer = static_cast<uint8_t*>(buffer_);
    uint32_t len = mContent.size();
    writeToBuffer<uint32_t>(len, buffer, remaining_buffer_size);
    TLLM_CHECK(len <= remaining_buffer_size);
    memcpy(buffer, mContent.c_str(), len);
}

CubinObj::CubinObj(std::string const& content)
    : mDriver(tensorrt_llm::common::CUDADriverWrapper::getInstance())
    , mContent(content)
    , mModule(nullptr)
    , mFunction(nullptr)
    , mSharedMemBytes(0)
{
    initialize(mContent.c_str(), "kernel_mha");
}

void CubinObj::launch(dim3 gridDim, dim3 blockDim, CUstream hStream, void** kernelParams)
{
    cuErrCheck(mDriver->cuLaunchKernel(mFunction, gridDim.x, gridDim.y, gridDim.z, blockDim.x, blockDim.y, blockDim.z,
                   mSharedMemBytes, hStream, kernelParams, /*extra=*/nullptr),
        mDriver);
}

void CubinObj::initialize(char const* content, char const* funcName)
{
    cuErrCheck(mDriver->cuModuleLoadData(&mModule, content), mDriver);
    TLLM_CHECK(mModule != nullptr);
    cuErrCheck(mDriver->cuModuleGetFunction(&mFunction, mModule, funcName), mDriver);
    TLLM_CHECK(mFunction != nullptr);

    // Populate mSharedMemBytes.
    CUdeviceptr shmem_dev_ptr = 0;
    cuErrCheck(mDriver->cuModuleGetGlobal(&shmem_dev_ptr, nullptr, mModule, "smemSize"), mDriver);
    TLLM_CHECK(shmem_dev_ptr != 0);
    cuErrCheck(mDriver->cuMemcpyDtoH(&mSharedMemBytes, shmem_dev_ptr, sizeof(unsigned int)), mDriver);

    TLLM_CHECK(mSharedMemBytes > 0);

    /* Set 46KB threshold here because we have to take static/driver shared memory into consideration. */
    if (mSharedMemBytes >= 46 * 1024)
    {
        cuErrCheck(
            mDriver->cuFuncSetAttribute(mFunction, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, mSharedMemBytes),
            mDriver);
    }

    sync_check_cuda_error();
}

} // namespace jit
} // namespace kernels
} // namespace tensorrt_llm
