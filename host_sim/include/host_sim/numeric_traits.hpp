#pragma once

#include "host_sim/q15.hpp"

#include <cmath>
#include <complex>
#include <cstdint>

namespace host_sim
{

struct FloatTraits
{
    using SampleType = float;
    using ComplexValue = std::complex<float>;

    static constexpr bool is_fixed_point = false;

    static SampleType to_sample(float value) { return value; }
    static float to_float(SampleType value) { return value; }
    static ComplexValue to_complex(const std::complex<float>& value) { return value; }
};

struct FixedQ15Traits
{
    using SampleType = std::int16_t;
    using ComplexValue = Q15Complex;

    static constexpr bool is_fixed_point = true;
    static constexpr int fractional_bits = 15;
    static constexpr float scale = static_cast<float>(1 << fractional_bits);

    static SampleType to_sample(float value)
    {
        float clamped = std::fmax(std::fmin(value, 0.999969f), -1.0f);
        return static_cast<SampleType>(std::lrint(clamped * scale));
    }

    static float to_float(SampleType value)
    {
        return static_cast<float>(value) / scale;
    }

    static ComplexValue to_complex(const std::complex<float>& value)
    {
        return float_to_q15_complex(value.real(), value.imag());
    }
};

using DefaultNumericTraits = FloatTraits;

} // namespace host_sim
