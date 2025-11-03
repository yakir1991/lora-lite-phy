#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace host_sim
{

constexpr int kQ15FractionalBits = 15;
constexpr int32_t kQ15Scale = 1 << kQ15FractionalBits;
constexpr int16_t kQ15Max = 0x7FFF;
constexpr int16_t kQ15Min = static_cast<int16_t>(0x8000);

struct Q15Complex
{
    int16_t real{0};
    int16_t imag{0};
};

inline int16_t saturate_q15(int32_t value)
{
    if (value > static_cast<int32_t>(kQ15Max)) {
        return kQ15Max;
    }
    if (value < static_cast<int32_t>(kQ15Min)) {
        return kQ15Min;
    }
    return static_cast<int16_t>(value);
}

inline int16_t float_to_q15(float value)
{
    const float clamped = std::clamp(value, -1.0f, 0.999969f);
    const int32_t scaled = static_cast<int32_t>(std::lrint(clamped * static_cast<float>(kQ15Scale)));
    return saturate_q15(scaled);
}

inline Q15Complex float_to_q15_complex(float real, float imag)
{
    return {float_to_q15(real), float_to_q15(imag)};
}

inline float q15_to_float(int16_t value)
{
    return static_cast<float>(value) / static_cast<float>(kQ15Scale);
}

inline Q15Complex q15_add(const Q15Complex& a, const Q15Complex& b)
{
    return {
        saturate_q15(static_cast<int32_t>(a.real) + static_cast<int32_t>(b.real)),
        saturate_q15(static_cast<int32_t>(a.imag) + static_cast<int32_t>(b.imag)),
    };
}

inline Q15Complex q15_sub(const Q15Complex& a, const Q15Complex& b)
{
    return {
        saturate_q15(static_cast<int32_t>(a.real) - static_cast<int32_t>(b.real)),
        saturate_q15(static_cast<int32_t>(a.imag) - static_cast<int32_t>(b.imag)),
    };
}

inline Q15Complex q15_mul(const Q15Complex& a, const Q15Complex& b)
{
    const int32_t ar = a.real;
    const int32_t ai = a.imag;
    const int32_t br = b.real;
    const int32_t bi = b.imag;

    const int32_t real = (ar * br - ai * bi) >> kQ15FractionalBits;
    const int32_t imag = (ar * bi + ai * br) >> kQ15FractionalBits;
    return {saturate_q15(real), saturate_q15(imag)};
}

} // namespace host_sim

