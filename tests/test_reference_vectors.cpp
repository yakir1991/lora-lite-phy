#include "lora/tx/loopback_tx.hpp"
#include "lora/rx/loopback_rx.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace lora;
using namespace lora::tx;
using namespace lora::rx;
using namespace lora::utils;

TEST(ReferenceVectors, CrossValidate) {
    namespace fs = std::filesystem;
    Workspace ws;
    auto root = fs::path(__FILE__).parent_path().parent_path();
    auto vec_dir = root / "vectors";
    for (const auto& entry : fs::directory_iterator(vec_dir)) {
        auto path = entry.path();
        auto name = path.filename().string();
        if (name.find("_payload.bin") == std::string::npos)
            continue;
        int sf = 0, cr_int = 0;
        if (sscanf(name.c_str(), "sf%d_cr%d_payload.bin", &sf, &cr_int) != 2)
            continue;
        CodeRate cr_enum;
        switch (cr_int) {
            case 45: cr_enum = CodeRate::CR45; break;
            case 46: cr_enum = CodeRate::CR46; break;
            case 47: cr_enum = CodeRate::CR47; break;
            case 48: cr_enum = CodeRate::CR48; break;
            default: continue;
        }
        std::ifstream pf(path, std::ios::binary);
        std::vector<uint8_t> payload((std::istreambuf_iterator<char>(pf)), {});
        std::string iq_name = (vec_dir / (
            "sf" + std::to_string(sf) + "_cr" + std::to_string(cr_int) + "_iq.bin")).string();
        std::ifstream iqf(iq_name, std::ios::binary);
        std::vector<std::complex<float>> iq;
        float buf[2];
        while (iqf.read(reinterpret_cast<char*>(buf), sizeof(buf)))
            iq.emplace_back(buf[0], buf[1]);
        SCOPED_TRACE(name);
        auto rx = lora::rx::loopback_rx(ws, iq, sf, cr_enum, payload.size());
        ASSERT_TRUE(rx.second);
        EXPECT_EQ(rx.first, payload);
        auto tx_iq = lora::tx::loopback_tx(ws, payload, sf, cr_enum);
        ASSERT_EQ(tx_iq.size(), iq.size());
        for (size_t i = 0; i < iq.size(); ++i) {
            EXPECT_NEAR(tx_iq[i].real(), iq[i].real(), 1e-3);
            EXPECT_NEAR(tx_iq[i].imag(), iq[i].imag(), 1e-3);
        }
    }
}
