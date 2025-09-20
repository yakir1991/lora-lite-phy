#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/complex.h>
#include <pybind11/numpy.h>
#include <complex>
#include <vector>
#include <string>
#include <optional>

#include "lora/rx/gr_pipeline.hpp"

namespace py = pybind11;

// Helper function to convert Python complex array to C++ vector
std::vector<std::complex<float>> numpy_to_complex_vector(py::array_t<std::complex<float>> input) {
    py::buffer_info buf_info = input.request();
    std::complex<float>* ptr = static_cast<std::complex<float>*>(buf_info.ptr);
    return std::vector<std::complex<float>>(ptr, ptr + buf_info.size);
}

// Helper function to convert C++ vector to Python list
py::list vector_to_python_list(const std::vector<uint8_t>& vec) {
    py::list result;
    for (const auto& item : vec) {
        result.append(item);
    }
    return result;
}

PYBIND11_MODULE(lora_pipeline, m) {
    m.doc() = "LoRa Pipeline Python Bindings";

    // Config class
    py::class_<lora::rx::pipeline::Config>(m, "Config")
        .def(py::init<>())
        .def_readwrite("sf", &lora::rx::pipeline::Config::sf)
        .def_readwrite("min_preamble_syms", &lora::rx::pipeline::Config::min_preamble_syms)
        .def_readwrite("symbols_after_preamble", &lora::rx::pipeline::Config::symbols_after_preamble)
        .def_readwrite("header_symbol_count", &lora::rx::pipeline::Config::header_symbol_count)
        .def_readwrite("os_candidates", &lora::rx::pipeline::Config::os_candidates)
        .def_readwrite("sto_search", &lora::rx::pipeline::Config::sto_search)
        .def_readwrite("expected_sync_word", &lora::rx::pipeline::Config::expected_sync_word)
        .def_readwrite("decode_payload", &lora::rx::pipeline::Config::decode_payload)
        .def_readwrite("expect_payload_crc", &lora::rx::pipeline::Config::expect_payload_crc)
        .def_readwrite("bandwidth_hz", &lora::rx::pipeline::Config::bandwidth_hz)
        .def_readwrite("ldro_override", &lora::rx::pipeline::Config::ldro_override);

    // FrameSyncOutput class
    py::class_<lora::rx::pipeline::FrameSyncOutput>(m, "FrameSyncOutput")
        .def(py::init<>())
        .def_readwrite("detected", &lora::rx::pipeline::FrameSyncOutput::detected)
        .def_readwrite("preamble_start_sample", &lora::rx::pipeline::FrameSyncOutput::preamble_start_sample)
        .def_readwrite("os", &lora::rx::pipeline::FrameSyncOutput::os)
        .def_readwrite("phase", &lora::rx::pipeline::FrameSyncOutput::phase)
        .def_readwrite("cfo", &lora::rx::pipeline::FrameSyncOutput::cfo)
        .def_readwrite("sto", &lora::rx::pipeline::FrameSyncOutput::sto)
        .def_readwrite("sync_detected", &lora::rx::pipeline::FrameSyncOutput::sync_detected)
        .def_readwrite("sync_start_sample", &lora::rx::pipeline::FrameSyncOutput::sync_start_sample)
        .def_readwrite("aligned_start_sample", &lora::rx::pipeline::FrameSyncOutput::aligned_start_sample)
        .def_readwrite("header_start_sample", &lora::rx::pipeline::FrameSyncOutput::header_start_sample);

    // HeaderStageOutput class
    py::class_<lora::rx::pipeline::HeaderStageOutput>(m, "HeaderStageOutput")
        .def(py::init<>())
        .def_readwrite("header_bytes", &lora::rx::pipeline::HeaderStageOutput::header_bytes)
        .def_readwrite("decoded_nibbles", &lora::rx::pipeline::HeaderStageOutput::decoded_nibbles)
        .def_readwrite("cw_bytes", &lora::rx::pipeline::HeaderStageOutput::cw_bytes);

    // PayloadStageOutput class
    py::class_<lora::rx::pipeline::PayloadStageOutput>(m, "PayloadStageOutput")
        .def(py::init<>())
        .def_readwrite("dewhitened_payload", &lora::rx::pipeline::PayloadStageOutput::dewhitened_payload)
        .def_readwrite("crc_ok", &lora::rx::pipeline::PayloadStageOutput::crc_ok);

    // PipelineResult class
    py::class_<lora::rx::pipeline::PipelineResult>(m, "PipelineResult")
        .def(py::init<>())
        .def_readwrite("success", &lora::rx::pipeline::PipelineResult::success)
        .def_readwrite("failure_reason", &lora::rx::pipeline::PipelineResult::failure_reason)
        .def_readwrite("frame_sync", &lora::rx::pipeline::PipelineResult::frame_sync)
        .def_readwrite("header", &lora::rx::pipeline::PipelineResult::header)
        .def_readwrite("payload", &lora::rx::pipeline::PipelineResult::payload);

    // GnuRadioLikePipeline class
    py::class_<lora::rx::pipeline::GnuRadioLikePipeline>(m, "GnuRadioLikePipeline")
        .def(py::init<lora::rx::pipeline::Config>())
        .def("run", [](lora::rx::pipeline::GnuRadioLikePipeline& self, py::array_t<std::complex<float>> samples) {
            auto samples_vec = numpy_to_complex_vector(samples);
            return self.run(samples_vec);
        });

    // Convenience function to run pipeline directly
    m.def("run_pipeline", [](py::array_t<std::complex<float>> samples, lora::rx::pipeline::Config config) {
        lora::rx::pipeline::GnuRadioLikePipeline pipeline(config);
        auto samples_vec = numpy_to_complex_vector(samples);
        return pipeline.run(samples_vec);
    }, "Run LoRa pipeline on complex samples", py::arg("samples"), py::arg("config"));
}
