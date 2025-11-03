#pragma once

#include "host_sim/fft_demod.hpp"
#include "host_sim/fft_demod_q15.hpp"
#include "host_sim/numeric_traits.hpp"
#include "host_sim/stage.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace host_sim
{

template <typename Traits = host_sim::FloatTraits>
class DemodStageT : public Stage
{
public:
    DemodStageT() = default;
    ~DemodStageT() override = default;

    void reset(const StageConfig& config) override
    {
        config_ = config;
        demod_ = std::make_unique<DemodulatorType>(config.sf, config.sample_rate, config.bandwidth);
        demod_->reset_symbol_counter();
        current_scale_ = 1.0f;
        if constexpr (Traits::is_fixed_point) {
            quantised_buffer_.clear();
            float_buffer_.clear();
        }
    }

    void process(SymbolContext& context) override
    {
        if (!demod_) {
            throw std::runtime_error("DemodStageT used before reset");
        }

        if constexpr (Traits::is_fixed_point) {
            quantised_buffer_.resize(context.samples.size());
            float_buffer_.resize(context.samples.size());
            float max_component = 0.0f;
            for (const auto& sample : context.samples) {
                max_component = std::max(max_component, std::max(std::abs(sample.real()), std::abs(sample.imag())));
            }
            current_scale_ = 1.0f;
            if (max_component > 1.0f) {
                current_scale_ = 1.0f / max_component;
            }
            for (std::size_t i = 0; i < context.samples.size(); ++i) {
                const auto scaled = context.samples[i] * current_scale_;
                quantised_buffer_[i] = Traits::to_complex(scaled);
                float_buffer_[i] = scaled;
            }
            demod_->set_input_scale(current_scale_);
            context.demod_symbol = demod_->demodulate(quantised_buffer_.data());
        } else {
            context.demod_symbol = demod_->demodulate(context.samples.data());
        }

        context.has_demod_symbol = true;
    }

    void flush() override {}

private:
    StageConfig config_{};
    using DemodulatorType =
        std::conditional_t<Traits::is_fixed_point, FftDemodulatorQ15, FftDemodulator>;
    std::unique_ptr<DemodulatorType> demod_;
    std::vector<typename Traits::ComplexValue> quantised_buffer_;
    std::vector<std::complex<float>> float_buffer_;
    float current_scale_{1.0f};
};

using DemodStage = DemodStageT<FloatTraits>;
using DemodStageQ15 = DemodStageT<FixedQ15Traits>;

} // namespace host_sim
