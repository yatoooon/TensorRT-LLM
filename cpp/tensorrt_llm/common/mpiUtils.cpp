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

#include "tensorrt_llm/common/mpiUtils.h"

#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/runtime/common.h"
#include "tensorrt_llm/runtime/iBuffer.h"

#include <csignal>
#include <cstdlib>
#include <mutex>
#include <type_traits>
#ifndef _WIN32
#include <unistd.h>
#endif

// We rely on SizeType32 being int32_t in some places with weak type checking,
// i.e. we're passing void ptr to some function. To prevent mysterious errors
// in the future, we trigger a compilation error here if SizeType32 isn't int32_t.
static_assert(std::is_same<tensorrt_llm::runtime::SizeType32, std::int32_t>::value);

namespace tensorrt_llm::mpi
{

MPI_Datatype getMpiDtype(MpiType dtype)
{
#if ENABLE_MULTI_DEVICE
    static std::unordered_map<MpiType, MPI_Datatype> const dtype_map{
        {MpiType::kBYTE, MPI_BYTE},
        {MpiType::kHALF, MPI_UINT16_T},
        {MpiType::kFLOAT, MPI_FLOAT},
        {MpiType::kDOUBLE, MPI_DOUBLE},
        {MpiType::kBOOL, MPI_C_BOOL},
        {MpiType::kINT8, MPI_INT8_T},
        {MpiType::kUINT8, MPI_UINT8_T},
        {MpiType::kINT32, MPI_INT32_T},
        {MpiType::kUINT32, MPI_UINT32_T},
        {MpiType::kINT64, MPI_INT64_T},
        {MpiType::kUINT64, MPI_UINT64_T},
        {MpiType::kFP8, MPI_UINT8_T},
        {MpiType::kBF16, MPI_UINT16_T},
        {MpiType::kCHAR, MPI_CHAR},
    };
    return dtype_map.at(dtype);
#else
    TLLM_THROW("Multi device support is disabled.");
#endif
}

MPI_Op getMpiOp(MpiOp op)
{
#if ENABLE_MULTI_DEVICE
    static std::unordered_map<MpiOp, MPI_Op> const op_map{
        {MpiOp::NULLOP, MPI_OP_NULL},
        {MpiOp::MAX, MPI_MAX},
        {MpiOp::MIN, MPI_MIN},
        {MpiOp::SUM, MPI_SUM},
        {MpiOp::PROD, MPI_PROD},
        {MpiOp::LAND, MPI_LAND},
        {MpiOp::BAND, MPI_BAND},
        {MpiOp::LOR, MPI_LOR},
        {MpiOp::BOR, MPI_BOR},
        {MpiOp::LXOR, MPI_LXOR},
        {MpiOp::BXOR, MPI_BXOR},
        {MpiOp::MINLOC, MPI_MINLOC},
        {MpiOp::MAXLOC, MPI_MAXLOC},
        {MpiOp::REPLACE, MPI_REPLACE},
    };
    return op_map.at(op);
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

namespace
{

bool mpiInitialized = false;
std::mutex mpiMutex;

} // namespace

void initialize(MpiThreadSupport threadMode, bool forwardAbortToParent)
{
    std::lock_guard<std::mutex> lk(mpiMutex);
    if (mpiInitialized)
    {
        return;
    }
#if ENABLE_MULTI_DEVICE
    int initialized = 0;
    TLLM_MPI_CHECK(MPI_Initialized(&initialized));
    if (!initialized)
    {
        TLLM_LOG_INFO("Initializing MPI with thread mode %d", threadMode);
        int providedMode;
        auto requiredMode = static_cast<int>(threadMode);
        MPICHECK(MPI_Init_thread(nullptr, nullptr, requiredMode, &providedMode));
        TLLM_CHECK_WITH_INFO(providedMode >= requiredMode, "MPI_Init_thread failed");
        std::atexit([]() { MPI_Finalize(); });

        /*
         * We only catch SIGABRT and SIGSEGV because most, of not all errors in the worker will cause one of these 2
         * signals. Signals like SIGINT and SIGTERM should be issued to the parent and should terminate MPI workers
         * correctly.
         */
        for (int sig : {SIGABRT, SIGSEGV})
        {
            __sighandler_t previousHandler = nullptr;
            if (forwardAbortToParent)
            {
                previousHandler = std::signal(sig,
                    [](int signal)
                    {
#ifndef _WIN32
                        pid_t parentProcessId = getppid();
                        kill(parentProcessId, SIGKILL);
#endif
                        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
                    });
            }
            else
            {
                previousHandler = std::signal(sig, [](int signal) { MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE); });
            }
            TLLM_CHECK_WITH_INFO(previousHandler != SIG_ERR, "Signal handler setup failed");
        }

        // ensure local MPI communicator is initialized
        MpiComm::localSession();
        TLLM_LOG_INFO("Initialized MPI");
    }
#endif // ENABLE_MULTI_DEVICE
    mpiInitialized = true;
}

void MpiComm::barrier() const
{
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Barrier(mComm));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

std::shared_ptr<MpiRequest> MpiComm::bcastAsync(void* buffer, size_t size, MpiType dtype, int root) const
{
    std::shared_ptr<MpiRequest> r = std::make_shared<MpiRequest>();
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Ibcast(buffer, size, getMpiDtype(dtype), root, mComm, &r->mRequest));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
    return r;
}

std::shared_ptr<MpiRequest> MpiComm::bcastAsync(runtime::IBuffer& buf, int root) const
{
    TLLM_CHECK(buf.getMemoryType() != runtime::MemoryType::kGPU);
    return bcastAsync(buf.data(), buf.getSizeInBytes(), MpiType::kBYTE, root);
}

void MpiComm::bcast(void* buffer, size_t size, MpiType dtype, int root) const
{
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Bcast(buffer, size, getMpiDtype(dtype), root, mComm));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

void MpiComm::bcast(runtime::IBuffer& buf, int root) const
{
    bcast(buf.data(), buf.getSizeInBytes(), MpiType::kBYTE, root);
}

std::shared_ptr<MpiRequest> MpiComm::sendAsync(void const* buffer, size_t size, MpiType dtype, int dest, int tag) const
{
    std::shared_ptr<MpiRequest> r = std::make_shared<MpiRequest>();
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Isend(buffer, size, getMpiDtype(dtype), dest, tag, mComm, &r->mRequest));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif
    return r;
}

std::shared_ptr<MpiRequest> MpiComm::sendAsync(runtime::IBuffer const& buf, int dest, int tag) const
{
    return sendAsync(buf.data(), buf.getSizeInBytes(), MpiType::kBYTE, dest, tag);
}

void MpiComm::send(void const* buffer, size_t size, MpiType dtype, int dest, int tag) const
{
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Send(buffer, size, getMpiDtype(dtype), dest, tag, mComm));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

void MpiComm::send(runtime::IBuffer const& buf, int dest, int tag) const
{
    send(buf.data(), buf.getSizeInBytes(), MpiType::kBYTE, dest, tag);
}

MPI_Status MpiComm::recv(void* buffer, size_t size, MpiType dtype, int source, int tag) const
{
    MPI_Status status{};
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Recv(buffer, size, getMpiDtype(dtype), source, tag, mComm, &status));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
    return status;
}

MPI_Status MpiComm::recv(runtime::IBuffer& buf, int source, int tag) const
{
    return recv(buf.data(), buf.getSizeInBytes(), MpiType::kBYTE, source, tag);
}

MpiComm MpiComm::split(int color, int key) const
{
    MPI_Comm splitComm;
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Comm_split(mComm, color, key, &splitComm));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
    return MpiComm{splitComm, true};
}

void MpiComm::allreduce(void const* sendbuf, void* recvbuf, int count, MpiType dtype, MpiOp op) const
{
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Allreduce(sendbuf, recvbuf, count, getMpiDtype(dtype), getMpiOp(op), mComm));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

void MpiComm::allgather(void const* sendbuf, void* recvbuf, int count, MpiType dtype) const
{
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Allgather(sendbuf, count, getMpiDtype(dtype), recvbuf, count, getMpiDtype(dtype), mComm));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

void MpiComm::mprobe(int source, int tag, MPI_Message* msg, MPI_Status* status) const
{
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Mprobe(source, tag, mComm, msg, status));
#else
    TLLM_THROW("Multi device support is disabled.");
#endif // ENABLE_MULTI_DEVICE
}

int MpiComm::getRank() const
{
    int rank = 0;
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Comm_rank(mComm, &rank));
#endif
    return rank;
}

int MpiComm::getSize() const
{
    int world_size = 1;
#if ENABLE_MULTI_DEVICE
    MPICHECK(MPI_Comm_size(mComm, &world_size));
#endif
    return world_size;
}

MpiComm const& MpiComm::world()
{
    static MpiComm commWorld{MPI_COMM_WORLD, false};
    return commWorld;
}

MpiComm& MpiComm::session()
{
    static MpiComm commSession{world(), false};
    return commSession;
}

MpiComm getLocalSession()
{
#if ENABLE_MULTI_DEVICE
    MPI_Comm localComm;
    MPI_Comm_split_type(MPI_COMM_WORLD, OMPI_COMM_TYPE_HOST, 0, MPI_INFO_NULL, &localComm);
    MpiComm localSession{localComm, false};
#else
    MpiComm localSession{MPI_COMM_WORLD, false};
#endif // ENABLE_MULTI_DEVICE
    return localSession;
}

MpiComm& MpiComm::localSession()
{
    static MpiComm localSession = getLocalSession();
    return localSession;
}

MpiComm::MpiComm(MPI_Comm g, bool freeComm)
    : mComm{g}
    , mFreeComm{freeComm}
{
    TLLM_CHECK(mComm != MPI_COMM_NULL);
    if (g == MPI_COMM_WORLD)
    {
        initialize();
    }
}

MpiComm::~MpiComm() noexcept
{
#if ENABLE_MULTI_DEVICE
    if (mFreeComm && mComm && MPI_Comm_free(&mComm) != MPI_SUCCESS)
    {
        TLLM_LOG_ERROR("MPI_Comm_free failed");
    }
#endif // ENABLE_MULTI_DEVICE
}

MpiComm::MpiComm(MpiComm&& comm) noexcept
    : mComm{comm.mComm}
    , mFreeComm{comm.mFreeComm}
{
    comm.mFreeComm = false;
}

MpiComm& MpiComm::operator=(MpiComm&& comm) noexcept
{
    this->~MpiComm();
    mComm = comm.mComm;
    mFreeComm = comm.mFreeComm;
    comm.mFreeComm = false;
    return *this;
}

} // namespace tensorrt_llm::mpi
