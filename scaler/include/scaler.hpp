// scaler/include/scaler.hpp
#pragma once

#include <cstdint>

namespace vkit {

    enum class ColorFormat {
        BGRA32,
        RGB24,
        RGB565,
        NV12
    };

    struct ScalerDesc {
        uint32_t src_width = 0;
        uint32_t src_height = 0;
        uint32_t dst_width = 0;
        uint32_t dst_height = 0;
        ColorFormat dst_format = ColorFormat::RGB24;
    };

    #if defined(_WIN32)
        // Forward declare the concrete implementation class so the alias knows it exists
        class D3D11Scaler;
    #endif

} // namespace vkit

// Platform selector alias
#if defined(_WIN32)
    #include "scaler_d3d11.hpp"
    namespace vkit {
        using Scaler = D3D11Scaler;
    }
#endif