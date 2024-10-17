/*
 * Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
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

#include "tensorrt_llm/executor/executor.h"

namespace tensorrt_llm
{
namespace runtime
{

class DecodingMode
{
public:
    static auto constexpr None()
    {
        return DecodingMode{kNone};
    }

    static auto constexpr TopK()
    {
        return DecodingMode{kTopK};
    }

    static auto constexpr TopP()
    {
        return DecodingMode{kTopP};
    }

    static auto constexpr TopKTopP()
    {
        return DecodingMode{kTopKTopP};
    }

    static auto constexpr BeamSearch()
    {
        return DecodingMode{kBeamSearch};
    }

    static auto constexpr Medusa()
    {
        return DecodingMode{kMedusa};
    }

    bool constexpr isNone() const
    {
        return mState == 0;
    }

    bool constexpr isTopK() const
    {
        return anyBitSet(kTopK);
    }

    bool constexpr isTopP() const
    {
        return anyBitSet(kTopP);
    }

    bool constexpr isTopKorTopP() const
    {
        return anyBitSet(kTopKTopP);
    }

    bool constexpr isTopKandTopP() const
    {
        return allBitSet(kTopKTopP);
    }

    bool constexpr isBeamSearch() const
    {
        return anyBitSet(kBeamSearch);
    }

    bool constexpr isMedusa() const
    {
        return anyBitSet(kMedusa);
    }

    using UnderlyingType = uint8_t;

    bool operator==(DecodingMode const& other) const
    {
        return mState == other.mState;
    }

    static DecodingMode fromExecutor(executor::DecodingMode decodingMode)
    {
        switch (decodingMode)
        {
        case executor::DecodingMode::kNONE: return DecodingMode::None();

        case executor::DecodingMode::kTOP_K: return DecodingMode::TopK();

        case executor::DecodingMode::kTOP_P: return DecodingMode::TopP();

        case executor::DecodingMode::kBEAM_SEARCH: return DecodingMode::BeamSearch();

        case executor::DecodingMode::kMEDUSA: return DecodingMode::Medusa();

        case executor::DecodingMode::kTOP_K_TOP_P: return DecodingMode::TopKTopP();

        default: TLLM_THROW("Invalid decoding mode"); break;
        }
    }

    friend std::ostream& operator<<(std::ostream& os, DecodingMode other);

private:
    constexpr DecodingMode(UnderlyingType state)
        : mState(state)
    {
    }

    // No mode specified. Config will be determined from the beam width of the first request at runtime
    // TopKTopP if beamWidth == 1, BeamSearch otherwise
    static UnderlyingType constexpr kNone{0};
    static UnderlyingType constexpr kTopK{1u << 0};
    static UnderlyingType constexpr kTopP{1u << 1};
    static UnderlyingType constexpr kBeamSearch{1u << 2};
    static UnderlyingType constexpr kMedusa{1u << 3};
    static UnderlyingType constexpr kTopKTopP{kTopK | kTopP};

    bool constexpr anyBitSet(UnderlyingType bits) const
    {
        return (mState & bits) != 0;
    }

    bool constexpr allBitSet(UnderlyingType bits) const
    {
        return (mState & bits) == bits;
    }

    UnderlyingType mState{};
};

static_assert(DecodingMode::None().isNone());
static_assert(!DecodingMode::None().isTopK());
static_assert(!DecodingMode::None().isTopP());
static_assert(!DecodingMode::None().isBeamSearch());
static_assert(!DecodingMode::None().isMedusa());

static_assert(DecodingMode::TopK().isTopK());
static_assert(DecodingMode::TopK().isTopKorTopP());
static_assert(!DecodingMode::TopK().isTopKandTopP());
static_assert(!DecodingMode::TopK().isTopP());
static_assert(!DecodingMode::TopK().isBeamSearch());
static_assert(!DecodingMode::TopK().isMedusa());

static_assert(DecodingMode::TopP().isTopP());
static_assert(DecodingMode::TopP().isTopKorTopP());
static_assert(!DecodingMode::TopP().isTopKandTopP());
static_assert(!DecodingMode::TopP().isTopK());
static_assert(!DecodingMode::TopP().isBeamSearch());
static_assert(!DecodingMode::TopP().isMedusa());

static_assert(DecodingMode::TopKTopP().isTopK());
static_assert(DecodingMode::TopKTopP().isTopP());
static_assert(DecodingMode::TopKTopP().isTopKorTopP());
static_assert(DecodingMode::TopKTopP().isTopKandTopP());
static_assert(!DecodingMode::TopKTopP().isBeamSearch());
static_assert(!DecodingMode::TopKTopP().isMedusa());

static_assert(DecodingMode::BeamSearch().isBeamSearch());
static_assert(!DecodingMode::BeamSearch().isTopKorTopP());
static_assert(!DecodingMode::BeamSearch().isMedusa());

static_assert(!DecodingMode::Medusa().isTopK());
static_assert(!DecodingMode::Medusa().isTopKorTopP());
static_assert(!DecodingMode::Medusa().isTopKandTopP());
static_assert(!DecodingMode::Medusa().isTopP());
static_assert(!DecodingMode::Medusa().isBeamSearch());
static_assert(DecodingMode::Medusa().isMedusa());

} // namespace runtime
} // namespace tensorrt_llm
