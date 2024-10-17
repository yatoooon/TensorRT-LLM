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

#include "tensorrt_llm/runtime/gptJsonConfig.h"

#include "common.h"
#include "modelConfig.h"
#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/common/stringUtils.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

using namespace tensorrt_llm::runtime;
namespace tc = tensorrt_llm::common;

namespace
{
using Json = typename nlohmann::json::basic_json;

template <typename FieldType>
FieldType parseJsonFieldOr(Json const& json, std::string_view name, FieldType defaultValue)
{
    auto value = defaultValue;
    try
    {
        value = json.at(name).template get<FieldType>();
    }
    catch (nlohmann::json::out_of_range& e)
    {
        TLLM_LOG_WARNING("Parameter %s cannot be read from json:", std::string(name).c_str());
        TLLM_LOG_WARNING(e.what());
    }
    return value;
}

template <typename FieldType>
std::optional<FieldType> parseJsonFieldOptional(Json const& json, std::string_view name)
{
    std::optional<FieldType> value = std::nullopt;
    try
    {
        value = json.at(name).template get<FieldType>();
    }
    catch (nlohmann::json::out_of_range const& e)
    {
        TLLM_LOG_WARNING(e.what());
        TLLM_LOG_WARNING("Optional value for parameter %s will not be set.", std::string(name).c_str());
    }
    catch (nlohmann::json::type_error const& e)
    {
        TLLM_LOG_WARNING(e.what());
        TLLM_LOG_WARNING("Optional value for parameter %s will not be set.", std::string(name).c_str());
    }
    return value;
}

std::vector<ModelConfig::LayerType> buildLayerTypes(
    std::size_t const numLayers, std::vector<std::string> const& layerStringTypes)
{
    std::vector<ModelConfig::LayerType> result{numLayers, ModelConfig::LayerType::kATTENTION};
    if (layerStringTypes.empty())
    {
        return result;
    }

    auto constexpr layerNameAttention = "attention";
    auto constexpr layerNameRecurrent = "recurrent";

    // The json field specifies a "group" of layers, which gets repeated multiple times
    // Note that the total number of layers does not need to be a multiple of a layer
    // group size (i.e. the last group will be incomplete).
    // For instance, Griffin has groups of 3 layers (2 recurrent + 1 attention) and 26
    // layers total (the last group has no attention layer)
    auto const groupSize = layerStringTypes.size();
    for (std::size_t i = 0; i < numLayers; ++i)
    {
        if (layerStringTypes[i % groupSize] == layerNameAttention)
        {
            result[i] = ModelConfig::LayerType::kATTENTION;
        }
        else if (layerStringTypes[i % groupSize] == layerNameRecurrent)
        {
            result[i] = ModelConfig::LayerType::kRECURRENT;
        }
        else
        {
            TLLM_LOG_ERROR("Unknown layer type: %s", layerStringTypes[i % groupSize].c_str());
        }
    }

    return result;
}

ModelConfig createModelConfig(
    Json const& json, bool engineVersionNone, SizeType32 tensorParallelism, nvinfer1::DataType dataType)
{
    auto const& config = engineVersionNone ? json.at("builder_config") : json.at("pretrained_config");

    auto const useCrossAttention = parseJsonFieldOptional<bool>(config, "cross_attention");
    auto const* const numLayersField = engineVersionNone ? "num_layers" : "num_hidden_layers";
    auto const* const numHeadsField = engineVersionNone ? "num_heads" : "num_attention_heads";
    auto const* const numKvHeadsField = engineVersionNone ? "num_kv_heads" : "num_key_value_heads";
    auto const* const mlpHiddenSizeField = engineVersionNone ? "mlp_hidden_size" : "intermediate_size";

    auto const numLayers = config.at(numLayersField).template get<SizeType32>();
    auto const numHeads = config.at(numHeadsField).template get<SizeType32>() / tensorParallelism;
    auto const layerStringTypes
        = parseJsonFieldOr<std::vector<std::string>>(config, "layer_types", std::vector<std::string>());
    auto const layerTypes = buildLayerTypes(numLayers, layerStringTypes);
    auto const numAttentionLayers
        = static_cast<SizeType32>(std::count(layerTypes.begin(), layerTypes.end(), ModelConfig::LayerType::kATTENTION));
    auto const numRnnLayers
        = static_cast<SizeType32>(std::count(layerTypes.begin(), layerTypes.end(), ModelConfig::LayerType::kRECURRENT));

    auto const vocabSize = config.at("vocab_size").template get<SizeType32>();
    auto const hiddenSize = config.at("hidden_size").template get<SizeType32>() / tensorParallelism;
    auto const sizePerHead = parseJsonFieldOr(config, "head_size", hiddenSize / numHeads);

    // TODO:
    // Code crashes when numKvHeads <= 0. Clamping downwards to 1 prevents that, make sure this is best fix.
    auto const numKvHeads
        = std::max(parseJsonFieldOr(config, numKvHeadsField, numHeads * tensorParallelism) / tensorParallelism, 1);

    auto const mlpHiddenSize = parseJsonFieldOptional<SizeType32>(config, mlpHiddenSizeField);

    auto modelConfig = ModelConfig{vocabSize, numAttentionLayers, numRnnLayers, numHeads, hiddenSize, dataType};
    modelConfig.setSizePerHead(sizePerHead);
    modelConfig.setNbKvHeads(numKvHeads);
    modelConfig.setLayerTypes(layerTypes);

    if (useCrossAttention.has_value())
    {
        modelConfig.useCrossAttention(useCrossAttention.value());
    }

    if (mlpHiddenSize.has_value())
    {
        modelConfig.setMlpHiddenSize(mlpHiddenSize.value() / tensorParallelism);
    }

    return modelConfig;
};

void parseBuilderConfig(ModelConfig& modelConfig, Json const& builderConfig)
{
    auto const maxBatchSize = parseJsonFieldOr(builderConfig, "max_batch_size", 0);
    auto const maxBeamWidth = parseJsonFieldOr(builderConfig, "max_beam_width", 0);
    auto const maxInputLen = parseJsonFieldOr(builderConfig, "max_input_len", 0);
    auto const maxSequenceLen = maxInputLen + parseJsonFieldOr(builderConfig, "max_output_len", 0);
    auto const maxDraftLen = parseJsonFieldOr(builderConfig, "max_draft_len", 0);
    auto const maxNumTokens = parseJsonFieldOptional<SizeType32>(builderConfig, "max_num_tokens");
    auto const maxPromptEmbeddingTableSize
        = parseJsonFieldOr<SizeType32>(builderConfig, "max_prompt_embedding_table_size", 0);
    auto const computeContextLogits = parseJsonFieldOr(builderConfig, "gather_context_logits", false);
    auto const computeGenerationLogits = parseJsonFieldOr(builderConfig, "gather_generation_logits", false);

    modelConfig.setMaxBatchSize(maxBatchSize);
    modelConfig.setMaxBeamWidth(maxBeamWidth);
    modelConfig.setMaxInputLen(maxInputLen);
    modelConfig.setMaxSequenceLen(maxSequenceLen);
    modelConfig.setMaxNumTokens(maxNumTokens);
    modelConfig.setMaxDraftLen(maxDraftLen);
    modelConfig.setMaxPromptEmbeddingTableSize(maxPromptEmbeddingTableSize);
    modelConfig.computeContextLogits(computeContextLogits);
    modelConfig.computeGenerationLogits(computeGenerationLogits);
}

void parsePluginConfig(ModelConfig& modelConfig, Json const& pluginConfig)
{
    auto const useGptAttentionPlugin = !pluginConfig.at("gpt_attention_plugin").is_null();
    auto const useMambaConv1dPlugin
        = pluginConfig.contains("mamba_conv1d_plugin") && !pluginConfig.at("mamba_conv1d_plugin").is_null();
    auto const removeInputPadding = pluginConfig.at("remove_input_padding").template get<bool>();
    auto const& pagedKvCache = pluginConfig.at("paged_kv_cache");
    auto const& tokensPerBlock = pluginConfig.at("tokens_per_block");
    auto const useCustomAllReduce = pluginConfig.at("use_custom_all_reduce").template get<bool>();
    auto const useContextFMHAForGeneration = pluginConfig.at("use_context_fmha_for_generation").template get<bool>();
    auto const pagedContextFMHA = pluginConfig.at("use_paged_context_fmha").template get<bool>();
    auto const pagedState = parseJsonFieldOr(pluginConfig, "paged_state", false);
    auto const useXQA = parseJsonFieldOr(pluginConfig, "enable_xqa", false);

    modelConfig.useGptAttentionPlugin(useGptAttentionPlugin);
    modelConfig.useMambaConv1dPlugin(useMambaConv1dPlugin);
    modelConfig.usePackedInput(removeInputPadding);
    modelConfig.usePagedKvCache(pagedKvCache);
    modelConfig.usePagedState(pagedState);
    modelConfig.setTokensPerBlock(tokensPerBlock);
    modelConfig.useCustomAllReduce(useCustomAllReduce);
    modelConfig.setUseContextFMHAForGeneration(useContextFMHAForGeneration);
    modelConfig.setPagedContextFMHA(pagedContextFMHA);
    modelConfig.useXQA(useXQA);
}

void parseLora(ModelConfig& modelConfig, Json const& json, Json const& pluginConfig, bool engineVersionNone,
    SizeType32 tensorParallelism)
{
    auto const& config = engineVersionNone ? json.at("builder_config") : json.at("build_config").at("lora_config");

    auto const loraMaxRank = parseJsonFieldOr(config, "max_lora_rank", SizeType32{0});
    auto const loraTargetModules = parseJsonFieldOptional<std::vector<std::string>>(config, "lora_target_modules");

    if (loraTargetModules.has_value())
    {

        modelConfig.setLoraModules(LoraModule::createLoraModules(loraTargetModules.value(), modelConfig.getHiddenSize(),
            modelConfig.getMlpHiddenSize(), modelConfig.getNbHeads(), modelConfig.getNbKvHeads(),
            modelConfig.getSizePerHead(), tensorParallelism));
    }

    modelConfig.setMaxLoraRank(loraMaxRank);

    auto useLoraPlugin = !pluginConfig.at("lora_plugin").is_null();
    if (useLoraPlugin)
    {

        if (modelConfig.getLoraModules().empty() || modelConfig.getMaxLoraRank() == 0)
        {
            TLLM_LOG_WARNING("lora_plugin enabled, but no lora module enabled: setting useLoraPlugin to false");
            useLoraPlugin = false;
        }
    }
    modelConfig.useLoraPlugin(useLoraPlugin);
}

template <typename InputType>
GptJsonConfig parseJson(InputType&& input)
{
    auto constexpr allowExceptions = true;
    auto constexpr ignoreComments = true;
    auto const json = nlohmann::json::parse(std::forward<InputType>(input), nullptr, allowExceptions, ignoreComments);

    auto const engineVersion = parseJsonFieldOr(json, "version", std::string("none"));

    auto const engineVersionNone = engineVersion == std::string("none");
    if (engineVersionNone)
    {
        TLLM_LOG_INFO("No engine version found in the config file, assuming engine(s) built by old builder API.");
    }
    else
    {
        TLLM_LOG_INFO("Engine version %s found in the config file, assuming engine(s) built by new builder API.",
            engineVersion.c_str());
    }

    auto const& builderConfig = engineVersionNone ? json.at("builder_config") : json.at("build_config");

    auto const name = engineVersionNone ? builderConfig.at("name").template get<std::string>()
                                        : json.at("pretrained_config").at("architecture").template get<std::string>();

    auto const tensorParallelism = engineVersionNone
        ? builderConfig.at("tensor_parallel").template get<SizeType32>()
        : json.at("pretrained_config").at("mapping").at("tp_size").template get<SizeType32>();
    auto const pipelineParallelism = engineVersionNone
        ? parseJsonFieldOr(builderConfig, "pipeline_parallel", 1)
        : parseJsonFieldOr(json.at("pretrained_config").at("mapping"), "pp_size", 1);
    auto const gpusPerNode = engineVersionNone ? WorldConfig::kDefaultGpusPerNode
                                               : parseJsonFieldOr(json.at("pretrained_config").at("mapping"),
                                                   "gpus_per_node", WorldConfig::kDefaultGpusPerNode);

    auto const precision = engineVersionNone ? builderConfig.at("precision").template get<std::string>()
                                             : json.at("pretrained_config").at("dtype").template get<std::string>();

    auto const dataType = [&precision]()
    {
        if (!precision.compare("float32"))
            return nvinfer1::DataType::kFLOAT;
        else if (!precision.compare("float16"))
            return nvinfer1::DataType::kHALF;
        else if (!precision.compare("bfloat16"))
            return nvinfer1::DataType::kBF16;
        else
            TLLM_THROW("Model data type '%s' not supported", precision.c_str());
    }();

    auto modelConfig = createModelConfig(json, engineVersionNone, tensorParallelism, dataType);

    parseBuilderConfig(modelConfig, builderConfig);

    auto const& pluginConfig = engineVersionNone ? json.at("plugin_config") : builderConfig.at("plugin_config");
    parsePluginConfig(modelConfig, pluginConfig);

    parseLora(modelConfig, json, pluginConfig, engineVersionNone, tensorParallelism);

    if (engineVersionNone)
    {
        auto const quantMode
            = tc::QuantMode(parseJsonFieldOr(builderConfig, "quant_mode", tc::QuantMode::none().value()));
        modelConfig.setQuantMode(quantMode);
    }
    else
    {
        auto const& quantization = json.at("pretrained_config").at("quantization");
        auto quantAlgo = parseJsonFieldOptional<std::string>(quantization, "quant_algo");
        auto kvCacheQuantAlgo = parseJsonFieldOptional<std::string>(quantization, "kv_cache_quant_algo");
        auto const quantMode = tc::QuantMode::fromQuantAlgo(quantAlgo, kvCacheQuantAlgo);
        modelConfig.setQuantMode(quantMode);
    }

    if (engineVersionNone)
    {
        if (name == std::string("chatglm_6b") || name == std::string("glm_10b"))
        {
            modelConfig.setModelVariant(ModelConfig::ModelVariant::kGlm);
            // kGlm is only for ChatGLM-6B and GLM-10B
        }
    }
    else
    {
        if (name == "ChatGLMForCausalLM")
        {
            auto const& pretrainedConfig = json.at("pretrained_config");
            auto const chatglmVersion = pretrainedConfig.at("chatglm_version").template get<std::string>();
            if (chatglmVersion == "glm" || chatglmVersion == "chatglm")
            {
                modelConfig.setModelVariant(ModelConfig::ModelVariant::kGlm);
                // kGlm is only for ChatGLM-6B and GLM-10B
            }
        }
    }

    if (!engineVersionNone)
    {
        auto const& pretrainedConfig = json.at("pretrained_config");
        auto const medusaHeads = parseJsonFieldOptional<SizeType32>(pretrainedConfig, "num_medusa_heads");
        auto const maxDraftLen = parseJsonFieldOptional<SizeType32>(pretrainedConfig, "max_draft_len");
        TLLM_CHECK_WITH_INFO((medusaHeads.has_value() ^ maxDraftLen.has_value()) == 0,
            "Either both num_medusa_heads and max_draft_len or none have to be provided");
        if (medusaHeads.has_value() && medusaHeads.value() > 0)
        {
            modelConfig.setMaxDraftLen(maxDraftLen.value());
            auto medusaModule = MedusaModule(medusaHeads.value(), maxDraftLen.value());
            modelConfig.setMedusaModule(medusaModule);
        }
    }

    // RNN config
    if (!engineVersionNone)
    {
        auto const& pretrainedConfig = json.at("pretrained_config");
        auto const architecture = pretrainedConfig.at("architecture").template get<std::string>();
        if (architecture == std::string("MambaForCausalLM"))
        {
            modelConfig.setModelVariant(ModelConfig::ModelVariant::kMamba);
        }
        else if (architecture == std::string("RecurrentGemmaForCausalLM"))
        {
            modelConfig.setModelVariant(ModelConfig::ModelVariant::kRecurrentGemma);
        }
        if (modelConfig.isRnnBased())
        {
            auto const& stateSize = pretrainedConfig.at("state_size").template get<SizeType32>();
            auto const& convKernel = pretrainedConfig.at("conv_kernel").template get<SizeType32>();
            auto const& rnnHiddenSize = pretrainedConfig.at("rnn_hidden_size").template get<SizeType32>();
            ModelConfig::RnnConfig rnnConfig{};
            rnnConfig.stateSize = stateSize;
            rnnConfig.convKernel = convKernel;
            rnnConfig.rnnHiddenSize = rnnHiddenSize;
            modelConfig.setRnnConfig(rnnConfig);
        }
    }
    else
    {
        if (name.size() >= 6 && name.substr(0, 6) == "mamba_")
        {
            modelConfig.setModelVariant(ModelConfig::ModelVariant::kMamba);
        }
        else if (name.size() >= 15 && name.substr(0, 15) == "recurrentgemma_")
        {
            modelConfig.setModelVariant(ModelConfig::ModelVariant::kRecurrentGemma);
        }
        if (modelConfig.isRnnBased())
        {
            auto const& stateSize = builderConfig.at("state_size").template get<SizeType32>();
            auto const& convKernel = builderConfig.at("conv_kernel").template get<SizeType32>();
            auto const& rnnHiddenSize = builderConfig.at("rnn_hidden_size").template get<SizeType32>();
            ModelConfig::RnnConfig rnnConfig{};
            rnnConfig.stateSize = stateSize;
            rnnConfig.convKernel = convKernel;
            rnnConfig.rnnHiddenSize = rnnHiddenSize;
            modelConfig.setRnnConfig(rnnConfig);
        }
    }
    return GptJsonConfig{
        name, engineVersion, precision, tensorParallelism, pipelineParallelism, gpusPerNode, modelConfig};
}

} // namespace

std::string GptJsonConfig::engineFilename(WorldConfig const& worldConfig, std::string const& model) const
{
    TLLM_CHECK_WITH_INFO(getTensorParallelism() == worldConfig.getTensorParallelism(), "tensor parallelism mismatch");
    TLLM_CHECK_WITH_INFO(
        getPipelineParallelism() == worldConfig.getPipelineParallelism(), "pipeline parallelism mismatch");
    auto pp = worldConfig.isPipelineParallel() ? "_pp" + std::to_string(worldConfig.getPipelineParallelism()) : "";
    if (getVersion() == std::string("none"))
    {
        return model + "_" + getPrecision() + "_tp" + std::to_string(worldConfig.getTensorParallelism()) + pp + "_rank"
            + std::to_string(worldConfig.getRank()) + ".engine";
    }
    else
    {
        return "rank" + std::to_string(worldConfig.getRank()) + ".engine";
    }
}

GptJsonConfig GptJsonConfig::parse(std::string const& json)
{
    return parseJson(json);
}

GptJsonConfig GptJsonConfig::parse(std::istream& json)
{
    return parseJson(json);
}

GptJsonConfig GptJsonConfig::parse(std::filesystem::path const& path)
{
    TLLM_CHECK_WITH_INFO(std::filesystem::exists(path), std::string("File does not exist: ") + path.string());
    std::ifstream json(path);
    return parse(json);
}
