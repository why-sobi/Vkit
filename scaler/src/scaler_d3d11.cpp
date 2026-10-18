// src/scaler_d3d11.cpp

#include "scaler_d3d11.hpp"
#include <iostream>

namespace vkit {

    DXGI_FORMAT D3D11Scaler::to_dxgi_format(ColorFormat format) {
        switch (format) {
            case ColorFormat::BGRA32: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case ColorFormat::NV12:   return DXGI_FORMAT_NV12;
            default:                  return DXGI_FORMAT_UNKNOWN; // RGB24 / RGB565 have no video processor output
        }
    }

    bool D3D11Scaler::init(ID3D11Device* device, const ScalerDesc& desc) {
        if (!device) return false;
        config_ = desc;

        // Drop everything tied to a previous device (re-init after a capture reset)
        video_context_.Reset();
        video_processor_.Reset();
        video_enum_.Reset();
        output_view_.Reset();
        scaled_texture_.Reset();

        output_format_ = to_dxgi_format(config_.dst_format);
        if (output_format_ == DXGI_FORMAT_UNKNOWN) {
            std::cerr << "[D3D11Scaler] Unsupported output format (use BGRA32 or NV12).\n";
            return false;
        }

        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&video_device_));
        if (FAILED(hr)) {
            std::cerr << "[D3D11Scaler] Device does not support ID3D11VideoDevice.\n";
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
        content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content_desc.InputWidth = config_.src_width;
        content_desc.InputHeight = config_.src_height;
        content_desc.OutputWidth = config_.dst_width;
        content_desc.OutputHeight = config_.dst_height;
        content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        hr = video_device_->CreateVideoProcessorEnumerator(&content_desc, video_enum_.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[D3D11Scaler] Failed to create Video Processor Enumerator.\n";
            return false;
        }

        UINT support = 0;
        if (FAILED(video_enum_->CheckVideoProcessorFormat(output_format_, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            std::cerr << "[D3D11Scaler] The video processor cannot output the requested format.\n";
            return false;
        }

        hr = video_device_->CreateVideoProcessor(video_enum_.Get(), 0, video_processor_.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[D3D11Scaler] Failed to create Video Processor.\n";
            return false;
        }

        return create_output_resources(device);
    }

    bool D3D11Scaler::create_output_resources(ID3D11Device* device) {
        D3D11_TEXTURE2D_DESC tex_desc{};

        tex_desc.Width = config_.dst_width;
        tex_desc.Height = config_.dst_height;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = output_format_;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, scaled_texture_.GetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc{};
        out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

        hr = video_device_->CreateVideoProcessorOutputView(
            scaled_texture_.Get(), 
            video_enum_.Get(), 
            &out_view_desc, 
            output_view_.GetAddressOf()
        );

        return SUCCEEDED(hr);
    }

    bool D3D11Scaler::scale_frame(ID3D11DeviceContext* context, 
                                  ID3D11Texture2D* src_texture, 
                                  ID3D11Texture2D* dst_texture) {
        if (!context || !src_texture) return false;

        if (!video_context_) {
            HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&video_context_));
            if (FAILED(hr)) return false;

            // Desktop frames are full-range RGB. YUV output is full-range BT.601,
            // the JFIF convention, so JPEG decoders show the right colors and levels.
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_space{};
            in_space.RGB_Range = 0; // 0-255
            video_context_->VideoProcessorSetStreamColorSpace(video_processor_.Get(), 0, &in_space);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_space{};
            out_space.RGB_Range = 0;
            out_space.YCbCr_Matrix = 0; // BT.601
            out_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            video_context_->VideoProcessorSetOutputColorSpace(video_processor_.Get(), &out_space);
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_view_desc{};
        in_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        in_view_desc.Texture2D.MipSlice = 0;
        in_view_desc.Texture2D.ArraySlice = 0;

        ComPtr<ID3D11VideoProcessorInputView> input_view;
        HRESULT hr = video_device_->CreateVideoProcessorInputView(
            src_texture, 
            video_enum_.Get(), 
            &in_view_desc, 
            input_view.GetAddressOf()
        );
        if (FAILED(hr)) return false;

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view.Get();

        hr = video_context_->VideoProcessorBlt(
            video_processor_.Get(), 
            output_view_.Get(), 
            0, 
            1, 
            &stream
        );

        if (SUCCEEDED(hr) && dst_texture) {
            context->CopyResource(dst_texture, scaled_texture_.Get());
        }

        return SUCCEEDED(hr);
    }

} // namespace vkit