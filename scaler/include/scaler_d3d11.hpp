// include/vkit/scaler_d3d11.hpp
#pragma once

#include "scaler.hpp"
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace vkit {

    class D3D11Scaler {
    private:
        ScalerDesc config_{};

        ComPtr<ID3D11VideoDevice> video_device_;
        ComPtr<ID3D11VideoContext> video_context_;
        ComPtr<ID3D11VideoProcessor> video_processor_;
        ComPtr<ID3D11VideoProcessorEnumerator> video_enum_;

        ComPtr<ID3D11Texture2D> scaled_texture_;
        ComPtr<ID3D11VideoProcessorOutputView> output_view_;

        bool create_output_resources(ID3D11Device* device);

    public:
        D3D11Scaler() = default;
        ~D3D11Scaler() = default;

        bool init(ID3D11Device* device, const ScalerDesc& desc);
        bool scale_frame(ID3D11DeviceContext* context, 
                         ID3D11Texture2D* src_texture, 
                         ID3D11Texture2D* dst_texture = nullptr);

        ScalerDesc get_desc() const { return config_; }
        ComPtr<ID3D11Texture2D> get_scaled_texture() const { return scaled_texture_; }
    };

} // namespace vkit