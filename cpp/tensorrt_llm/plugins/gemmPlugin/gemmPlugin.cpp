/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2022 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "gemmPlugin.h"

#include "gemmPluginProfiler.h"
#include "plugin.h"
#include "pluginUtils.h"

#include <NvInferRuntime.h>

#include <cassert>

using namespace nvinfer1;
using namespace tensorrt_llm::common;
using tensorrt_llm::plugins::GemmDims;
using tensorrt_llm::plugins::GemmPluginCreator;
using tensorrt_llm::plugins::GemmPlugin;
using tensorrt_llm::plugins::CublasLtGemmPluginProfiler;
using tensorrt_llm::plugins::CublasGemmWrapperPtr;
using tensorrt_llm::plugins::read;
using tensorrt_llm::plugins::write;

static char const* GEMM_PLUGIN_VERSION{"1"};
static char const* GEMM_PLUGIN_NAME{"Gemm"};
PluginFieldCollection GemmPluginCreator::mFC{};
std::vector<nvinfer1::PluginField> GemmPluginCreator::mPluginAttributes;

void getProblemParams(cublasOperation_t& transa, cublasOperation_t& transb, int& m, int& n, int& k, int& lda, int& ldb,
    int& ldc, bool transA, bool transB, int M, int N, int K, int padLda, int padLdb)
{
    transa = transB ? CUBLAS_OP_T : CUBLAS_OP_N;
    transb = transA ? CUBLAS_OP_T : CUBLAS_OP_N;
    m = N;
    n = M;
    k = K;
    lda = transB ? K + padLdb : N + padLdb;
    ldb = transA ? M + padLda : K + padLda;
    ldc = N;
}

void runGemm(int const M, int const N, int const K, bool const transA, bool const transB, int const padLda,
    int const padLdb, nvinfer1::DataType const type, CublasGemmWrapperPtr const& cublasWrapperPtr, void const* act,
    void const* weight, void* output, std::optional<cublasLtMatmulHeuristicResult_t> const& heuristic, void* workspace,
    cudaStream_t stream)
{
    if (M == 0 || N == 0 || K == 0)
        return;

    cublasWrapperPtr->setStream(stream);
    cublasWrapperPtr->setWorkspace(workspace);

    cublasOperation_t transa, transb;
    int m, n, k;
    int lda, ldb, ldc;
    getProblemParams(transa, transb, m, n, k, lda, ldb, ldc, transA, transB, M, N, K, padLda, padLdb);

    cublasWrapperPtr->createDescriptors(transa, transb, m, n, k, lda, ldb, ldc);
    cublasWrapperPtr->Gemm(transa, transb, m, n, k, weight, lda, act, ldb, output, ldc, heuristic);
    cublasWrapperPtr->destroyDescriptors();
}

void CublasLtGemmPluginProfiler::runTactic(
    int m, int n, int k, CublasLtGemmPluginProfiler::Config const& tactic, char* workspace, cudaStream_t const& stream)
{
    size_t dataSize = sizeof(half);
    if (mType == DataType::kFLOAT)
    {
        dataSize = sizeof(float);
    }

    void* actPtr = reinterpret_cast<void*>(workspace);
    void* weightPtr = reinterpret_cast<void*>(
        nextWorkspacePtrWithAlignment(reinterpret_cast<int8_t*>(actPtr), m * k * dataSize, ALIGNMENT));
    void* outputPtr = reinterpret_cast<void*>(
        nextWorkspacePtrWithAlignment(reinterpret_cast<int8_t*>(weightPtr), n * k * dataSize, ALIGNMENT));
    char* workspacePtr = reinterpret_cast<char*>(
        nextWorkspacePtrWithAlignment(reinterpret_cast<int8_t*>(outputPtr), m * n * dataSize, ALIGNMENT));
    runGemm(m, n, k, mTransA, mTransB, mPadLda, mPadLdb, mType, mRunner, actPtr, weightPtr, outputPtr, {tactic},
        workspacePtr, stream);
}

bool CublasLtGemmPluginProfiler::checkTactic(int m, int n, int k, Config const& tactic) const
{
    cublasOperation_t transa, transb;
    int M = m, N = n, K = k;
    int lda, ldb, ldc;
    getProblemParams(transa, transb, m, n, k, lda, ldb, ldc, mTransA, mTransB, M, N, K, mPadLda, mPadLdb);

    mRunner->createDescriptors(transa, transb, m, n, k, lda, ldb, ldc);

    auto const checkResult = mRunner->checkTactic(transa, transb, m, n, k, lda, ldb, ldc, tactic.algo);

    mRunner->destroyDescriptors();

    return checkResult;
}

void CublasLtGemmPluginProfiler::computeTmpSize(int maxM, int n, int k)
{
    size_t dataSize = typeSize(mType);
    size_t outputDataSize = typeSize(mOutputType);

    std::vector<size_t> workspaces = {
        maxM * k * dataSize,       // A
        n * k * dataSize,          // B
        maxM * n * outputDataSize, // C
        CUBLAS_WORKSPACE_SIZE      // workspace
    };
    size_t bytes = calculateTotalWorkspaceSize(workspaces.data(), workspaces.size(), ALIGNMENT);
    setTmpWorkspaceSizeInBytes(bytes);
}

std::vector<CublasLtGemmPluginProfiler::Config> CublasLtGemmPluginProfiler::getTactics(int M, int N, int K) const
{
    cublasOperation_t transa, transb;
    int m, n, k;
    int lda, ldb, ldc;
    getProblemParams(transa, transb, m, n, k, lda, ldb, ldc, mTransA, mTransB, M, N, K, mPadLda, mPadLdb);

    mRunner->createDescriptors(transa, transb, m, n, k, lda, ldb, ldc);
    auto const heruistics = mRunner->getTactics(transa, transb, m, n, k, lda, ldb, ldc);
    mRunner->destroyDescriptors();

    return heruistics;
}

GemmPlugin::GemmPlugin(int transA, int transB, int padLda, int padLdb, nvinfer1::DataType type, bool useFp8,
    GemmPlugin::PluginProfilerPtr const& pluginProfiler)
    : mTransA(transA)
    , mTransB(transB)
    , mPadLda(padLda)
    , mPadLdb(padLdb)
    , mType(type)
    , mUseFp8(useFp8)
    , mPluginProfiler(pluginProfiler)
    , mOutputType(type)
{
    init();
}

// Parameterized constructor
GemmPlugin::GemmPlugin(void const* data, size_t length, GemmPlugin::PluginProfilerPtr const& pluginProfiler)
    : mPluginProfiler(pluginProfiler)
{
    char const *d = reinterpret_cast<char const*>(data), *a = d;
    read(d, mTransA);
    read(d, mTransB);
    read(d, mPadLda);
    read(d, mPadLdb);
    read(d, mType);
    read(d, mUseFp8);
    read(d, mDims);
    read(d, mOutputType);

    init();

    mPluginProfiler->deserialize(d, mDims, mGemmId);

    TLLM_CHECK_WITH_INFO(d == a + length,
        "Expected length (%d) != real length (%d). This is often "
        "caused by using different TensorRT-LLM version to build "
        "engine and run engine.",
        (int) length, (int) (d - a));
}

void GemmPlugin::init()
{
    auto cublasHandle = getCublasHandle();
    auto cublasLtHandle = getCublasLtHandle();
    mCublasWrapper = std::make_shared<CublasMMWrapper>(cublasHandle, cublasLtHandle, nullptr, nullptr);

    mPluginProfiler->setTranspose(mTransA, mTransB);
    mPluginProfiler->setOutputType(mOutputType);
    mPluginProfiler->setPadLd(mPadLda, mPadLdb);

    mGemmId = GemmIdCublas(mDims.n, mDims.k, mType, mTransA, mTransB, mOutputType);
}

void GemmPlugin::setGemmConfig()
{
    if (mType == DataType::kHALF)
    {
        mCublasWrapper->setFP16GemmConfig(trtToCublasDtype(mOutputType));
    }
    else if (mType == DataType::kFLOAT)
    {
        mCublasWrapper->setFP32GemmConfig();
    }
#ifdef ENABLE_BF16
    else if (mType == DataType::kBF16)
    {
        mCublasWrapper->setBF16GemmConfig(trtToCublasDtype(mOutputType));
    }
#endif

#ifdef ENABLE_FP8
    if (mUseFp8)
    {
        mCublasWrapper->setFP8GemmConfig(trtToCublasDtype(mOutputType));
    }
#endif
}

void GemmPlugin::configGemm()
{
    if (!mDims.isInitialized())
    {
        return;
    }

    setGemmConfig();

    mPluginProfiler->profileTactics(mCublasWrapper, mType, mDims, mGemmId);
}

// IPluginV2DynamicExt Methods
nvinfer1::IPluginV2DynamicExt* GemmPlugin::clone() const noexcept
{
    auto* plugin = new GemmPlugin(*this);
    return plugin;
}

nvinfer1::DimsExprs GemmPlugin::getOutputDimensions(
    int outputIndex, nvinfer1::DimsExprs const* inputs, int nbInputs, nvinfer1::IExprBuilder& exprBuilder) noexcept
{
    try
    {
        TLLM_CHECK(nbInputs == 2);
        TLLM_CHECK(outputIndex == 0);
        int const nbDimsA = inputs[0].nbDims;
        int const nbDimsB = inputs[1].nbDims;
        DimsExprs ret;
        ret.nbDims = nbDimsA + nbDimsB - 2;

        if (mTransA)
        {
            for (int i = 1; i < nbDimsA; ++i)
            {
                ret.d[i - 1] = inputs[0].d[i];
            }
        }
        else
        {
            for (int i = 0; i < nbDimsA - 1; ++i)
            {
                ret.d[i] = inputs[0].d[i];
            }
        }
        if (mTransB)
        {
            for (int i = 0; i < nbDimsB - 1; ++i)
            {
                ret.d[nbDimsA - 1 + i] = inputs[1].d[i];
            }
        }
        else
        {
            for (int i = 1; i < nbDimsB; ++i)
            {
                ret.d[nbDimsA - 2 + i] = inputs[1].d[i];
            }
        }
        return ret;
    }
    catch (std::exception const& e)
    {
        caughtError(e);
    }
    return DimsExprs{};
}

bool GemmPlugin::supportsFormatCombination(
    int pos, nvinfer1::PluginTensorDesc const* inOut, int nbInputs, int nbOutputs) noexcept
{
    auto const& desc = inOut[pos];
    if (desc.format != TensorFormat::kLINEAR)
    {
        return false;
    }

    if (pos < nbInputs)
    {
        return desc.type == mType;
    }

    return desc.type == mType || desc.type == nvinfer1::DataType::kFLOAT;
}

void GemmPlugin::configurePlugin(nvinfer1::DynamicPluginTensorDesc const* in, int nbInputs,
    nvinfer1::DynamicPluginTensorDesc const* out, int nbOutputs) noexcept
{
    auto const nbDimsA = in[0].max.nbDims;

    auto const minM = utils::computeMDimension(mTransA, in[0].min);
    auto const maxM = utils::computeMDimension(mTransA, in[0].max);
    auto const N = utils::computeNDimension(mTransB, in[1].max);
    auto const K = static_cast<utils::DimType64>(mTransA ? in[0].max.d[0] : in[0].max.d[nbDimsA - 1]);

    if (!mDims.isInitialized())
    {
        mDims = {minM, maxM, N, K};
    }
    mGemmId.n = N;
    mGemmId.k = K;

    mOutputType = out[0].desc.type;
}

size_t GemmPlugin::getWorkspaceSize(nvinfer1::PluginTensorDesc const* inputs, int nbInputs,
    nvinfer1::PluginTensorDesc const* outputs, int nbOutputs) const noexcept
{
    return CUBLAS_WORKSPACE_SIZE;
}

int GemmPlugin::enqueue(nvinfer1::PluginTensorDesc const* inputDesc, nvinfer1::PluginTensorDesc const* outputDesc,
    void const* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream) noexcept
{
    // inputs
    //     mat1 [M, K] (mTransA = False)
    //     mat2 [K, N] (mTransB = False)
    // outputs
    //     mat [M, N]

    setGemmConfig();

    int const nbDimsA = inputDesc[0].dims.nbDims;
    int const padM = mTransA ? mPadLda : 0;
    int const padN = mTransB ? 0 : mPadLdb;
    int const padK = mTransA ? 0 : mPadLda;
    auto const M = utils::computeMDimension(mTransA, inputDesc[0].dims) - padM;
    auto const N = utils::computeNDimension(mTransB, inputDesc[1].dims) - padN;
    int const K = static_cast<utils::DimType64>(
        mTransA ? inputDesc[0].dims.d[0] - padK : inputDesc[0].dims.d[nbDimsA - 1] - padK);

    auto bestTactic = mPluginProfiler->getBestConfig(M, mGemmId);
    runGemm(M, N, K, mTransA, mTransB, mPadLda, mPadLdb, mType, mCublasWrapper, inputs[0], inputs[1], outputs[0],
        bestTactic, workspace, stream);
    return 0;
}

// IPluginV2Ext Methods
nvinfer1::DataType GemmPlugin::getOutputDataType(
    int index, nvinfer1::DataType const* inputTypes, int nbInputs) const noexcept
{
    TLLM_CHECK(index == 0);
    return inputTypes[0];
}

// IPluginV2 Methods

char const* GemmPlugin::getPluginType() const noexcept
{
    return GEMM_PLUGIN_NAME;
}

char const* GemmPlugin::getPluginVersion() const noexcept
{
    return GEMM_PLUGIN_VERSION;
}

int GemmPlugin::getNbOutputs() const noexcept
{
    return 1;
}

int GemmPlugin::initialize() noexcept
{
    configGemm();
    return 0;
}

void GemmPlugin::destroy() noexcept
{
    delete this;
}

size_t GemmPlugin::getSerializationSize() const noexcept
{
    return sizeof(mTransA) + sizeof(mTransB) + sizeof(mPadLda) + sizeof(mPadLdb) + sizeof(mType) + sizeof(mDims)
        + sizeof(mUseFp8) + mPluginProfiler->getSerializationSize(mGemmId)
        + sizeof(mOutputType); // selected tactics container size
}

void GemmPlugin::serialize(void* buffer) const noexcept
{
    char *d = static_cast<char*>(buffer), *a = d;
    write(d, mTransA);
    write(d, mTransB);
    write(d, mPadLda);
    write(d, mPadLdb);
    write(d, mType);
    write(d, mUseFp8);
    write(d, mDims);
    write(d, mOutputType);
    mPluginProfiler->serialize(d, mGemmId);

    assert(d == a + getSerializationSize());
}

void GemmPlugin::terminate() noexcept {}

///////////////

GemmPluginCreator::GemmPluginCreator()
{
    // Fill PluginFieldCollection with PluginField arguments metadata
    mPluginAttributes.clear();
    mPluginAttributes.emplace_back(PluginField("transA", nullptr, PluginFieldType::kINT32, 0));
    mPluginAttributes.emplace_back(PluginField("transB", nullptr, PluginFieldType::kINT32, 0));
    mPluginAttributes.emplace_back(PluginField("padLda", nullptr, PluginFieldType::kINT32, 0));
    mPluginAttributes.emplace_back(PluginField("padLdb", nullptr, PluginFieldType::kINT32, 0));
    mPluginAttributes.emplace_back(PluginField("type_id", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("use_fp8", nullptr, PluginFieldType::kINT32, 0));
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

char const* GemmPluginCreator::getPluginName() const noexcept
{
    return GEMM_PLUGIN_NAME;
}

char const* GemmPluginCreator::getPluginVersion() const noexcept
{
    return GEMM_PLUGIN_VERSION;
}

PluginFieldCollection const* GemmPluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2* GemmPluginCreator::createPlugin(char const* name, PluginFieldCollection const* fc) noexcept
{
    PluginField const* fields = fc->fields;
    int transA, transB, padLda, padLdb;
    nvinfer1::DataType type;
    int useFp8;
    // Read configurations from each fields
    for (int i = 0; i < fc->nbFields; ++i)
    {
        char const* attrName = fields[i].name;
        if (!strcmp(attrName, "transa"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            transA = static_cast<int>(*(static_cast<int const*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "transb"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            transB = static_cast<int>(*(static_cast<int const*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "pad_lda"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            padLda = static_cast<int>(*(static_cast<int const*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "pad_ldb"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            padLdb = static_cast<int>(*(static_cast<int const*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "type_id"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            type = static_cast<nvinfer1::DataType>(*(static_cast<nvinfer1::DataType const*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "use_fp8"))
        {
            TLLM_CHECK(fields[i].type == PluginFieldType::kINT32);
            useFp8 = static_cast<int>(*(static_cast<int const*>(fields[i].data)));
        }
    }
    try
    {
        // GemmPluginCreator is unique and shared for an engine generation
        // Create plugin profiler with shared tactics map
        // FIXME enable tactic profiler
        auto pluginProfiler = gemmPluginProfileManager.createGemmPluginProfiler(/* inference */ false, /* skip */ true);
        auto* obj = new GemmPlugin(transA, transB, padLda, padLdb, type, useFp8, pluginProfiler);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (std::exception const& e)
    {
        caughtError(e);
    }
    return nullptr;
}

IPluginV2* GemmPluginCreator::deserializePlugin(char const* name, void const* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call GemmPlugin::destroy()
    try
    {
        // GemmPluginCreator is unique and shared for an engine generation
        // Create plugin profiler with shared tactics map
        // FIXME enable tactic profiler
        auto pluginProfiler = gemmPluginProfileManager.createGemmPluginProfiler(/* inference */ true, /* skip */ true);
        auto* obj = new GemmPlugin(serialData, serialLength, pluginProfiler);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (std::exception const& e)
    {
        caughtError(e);
    }
    return nullptr;
}
