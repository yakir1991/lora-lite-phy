#include "host_sim/numeric_traits.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    using Float = host_sim::FloatTraits;
    using Fixed = host_sim::FixedQ15Traits;

    // Float mode round-trip
    const float f_value = 0.42f;
    const auto float_sample = Float::to_sample(f_value);
    if (std::fabs(Float::to_float(float_sample) - f_value) > 1e-6f) {
        std::cerr << "FloatTraits round-trip failed\n";
        return 1;
    }

    // Fixed mode saturation and round-trip (approximate)
    const float fixed_input = 0.5f;
    const auto fixed_sample = Fixed::to_sample(fixed_input);
    const float fixed_back = Fixed::to_float(fixed_sample);
    if (std::fabs(fixed_back - fixed_input) > 1.0f / Fixed::scale) {
        std::cerr << "FixedQ15Traits round-trip exceeded tolerance\n";
        return 1;
    }

    // Saturation at bounds
    if (Fixed::to_sample(2.0f) != Fixed::to_sample(1.0f)) {
        std::cerr << "FixedQ15Traits failed to clamp positive overflow\n";
        return 1;
    }
    if (Fixed::to_sample(-2.0f) != Fixed::to_sample(-1.0f)) {
        std::cerr << "FixedQ15Traits failed to clamp negative overflow\n";
        return 1;
    }

    return 0;
}
