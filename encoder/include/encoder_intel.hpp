// encoder/include/encoder_intel.hpp
#pragma once

#include "encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <vpl/mfx.h>
#include <wrl/client.h>

namespace vkit {

    // Intel oneVPL backend (hardware JPEG encode through the VPL dispatcher).
    //
    // With a D3D11 device, frames stay in video memory: the runtime reads the
    // caller's texture directly (or a GPU-side copy of it when the texture's
    // layout is not usable as-is). This needs an application frame allocator,
    // which is what the D3D11 allocator below provides. If the runtime rejects
    // video memory, the encoder falls back to system memory, reading textures
    // back through a staging texture.
    class IntelJpegEncoder {
    private:
        // One D3D11 resource handed to the runtime through the allocator.
        struct VideoFrame {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // video surface
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;     // P8 (bitstream) buffer
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; // CPU view used by Lock()
            mfxFrameInfo info{};
        };

        // Frames created by one Alloc() call; response.mids points into mids.
        struct Allocation {
            std::vector<std::unique_ptr<VideoFrame>> frames;
            std::vector<mfxMemId> mids;
        };

        JpegEncoderDesc desc_{};

        mfxLoader loader_ = nullptr;
        mfxSession session_ = nullptr;
        mfxFrameSurface1 surface_{};          // BGRA (MFX_FOURCC_RGB4) input frame
        mfxBitstream bitstream_{};            // compressed JPEG output
        std::vector<uint8_t> surface_data_;   // system-memory backing for surface_
        std::vector<uint8_t> bitstream_data_; // backing memory for bitstream_
        bool initialized_ = false;
        bool video_memory_ = false;           // true: surface_ is a D3D11 texture

        Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_; // system-memory fallback

        // Video-memory mode only.
        mfxFrameAllocator allocator_{};
        std::list<Allocation> allocations_;
        VideoFrame input_frame_;    // encoder-owned input, filled by a GPU copy
        VideoFrame external_frame_; // wraps the caller's texture for zero-copy input

        bool open_session(bool video_memory);
        void close_session();
        bool create_input_texture();
        bool create_staging_texture(const D3D11_TEXTURE2D_DESC& src_desc);
        bool encode_surface(std::vector<uint8_t>& jpeg_output);

        mfxStatus alloc_frames(const mfxFrameAllocRequest& request, mfxFrameAllocResponse& response);
        mfxStatus free_frames(mfxFrameAllocResponse& response);
        mfxStatus lock_frame(VideoFrame& frame, mfxFrameData& data);
        mfxStatus unlock_frame(VideoFrame& frame, mfxFrameData& data);

        static mfxStatus MFX_CDECL alloc_cb(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
        static mfxStatus MFX_CDECL lock_cb(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
        static mfxStatus MFX_CDECL unlock_cb(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
        static mfxStatus MFX_CDECL get_hdl_cb(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);
        static mfxStatus MFX_CDECL free_cb(mfxHDL pthis, mfxFrameAllocResponse* response);

    public:
        IntelJpegEncoder() = default;
        ~IntelJpegEncoder();

        IntelJpegEncoder(const IntelJpegEncoder&) = delete;
        IntelJpegEncoder& operator=(const IntelJpegEncoder&) = delete;

        // CPU-input mode. Kept for the standalone encoder test and fallback uses.
        bool init(const JpegEncoderDesc& desc);

        // GPU-input mode. d3d_device must be the same D3D11 device that owns the
        // texture passed to encode_texture(), and must be on the Intel adapter.
        bool init(ID3D11Device* d3d_device, const JpegEncoderDesc& desc);

        bool encode_bgra(const uint8_t* pixels,
                         size_t row_pitch,
                         std::vector<uint8_t>& jpeg_output);

        // Requires the D3D11 init() overload. The texture must be B8G8R8A8.
        bool encode_texture(ID3D11Texture2D* texture,
                            std::vector<uint8_t>& jpeg_output);

        void cleanup();

        JpegEncoderDesc get_desc() const { return desc_; }
        bool uses_video_memory() const { return video_memory_; }
    };

} // namespace vkit
