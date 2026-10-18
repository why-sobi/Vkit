// src/encoder_intel.cpp

#include "encoder_intel.hpp"

#include <d3d11_4.h> // ID3D11Multithread

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace vkit {

    namespace {

        constexpr uint32_t align16(uint32_t value) {
            return (value + 15u) & ~15u;
        }

        // Negative statuses are errors; positive ones are warnings the encoder
        // can continue through (e.g. MFX_WRN_PARTIAL_ACCELERATION).
        bool check(mfxStatus status, const char* operation) {
            if (status >= MFX_ERR_NONE) return true;
            std::cerr << "[IntelJpegEncoder] " << operation
                      << " failed with mfxStatus " << static_cast<int>(status) << '\n';
            return false;
        }

        bool set_filter_u32(mfxLoader loader, const char* name, mfxU32 value) {
            mfxConfig config = MFXCreateConfig(loader);
            if (!config) return false;

            mfxVariant variant{};
            variant.Type = MFX_VARIANT_TYPE_U32;
            variant.Data.U32 = value;
            return check(MFXSetConfigFilterProperty(
                             config, reinterpret_cast<const mfxU8*>(name), variant),
                         name);
        }

        DXGI_FORMAT to_dxgi_format(mfxU32 fourcc) {
            switch (fourcc) {
                case MFX_FOURCC_RGB4: return DXGI_FORMAT_B8G8R8A8_UNORM;
                case MFX_FOURCC_NV12: return DXGI_FORMAT_NV12;
                case MFX_FOURCC_YUY2: return DXGI_FORMAT_YUY2;
                case MFX_FOURCC_AYUV: return DXGI_FORMAT_AYUV;
                case MFX_FOURCC_P8:   return DXGI_FORMAT_P8;
                default:              return DXGI_FORMAT_UNKNOWN;
            }
        }

        void set_pitch(mfxFrameData& data, uint32_t pitch) {
            data.PitchHigh = static_cast<mfxU16>(pitch >> 16);
            data.PitchLow = static_cast<mfxU16>(pitch & 0xFFFF);
        }

        // Map can report the GPU is still using the resource; retry until it is free.
        HRESULT map_for_read(ID3D11DeviceContext* context, ID3D11Resource* resource,
                             D3D11_MAPPED_SUBRESOURCE& mapped) {
            HRESULT hr;
            do {
                hr = context->Map(resource, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) std::this_thread::yield();
            } while (hr == DXGI_ERROR_WAS_STILL_DRAWING);
            return hr;
        }

    } // namespace

    IntelJpegEncoder::~IntelJpegEncoder() {
        cleanup();
    }

    bool IntelJpegEncoder::init(const JpegEncoderDesc& desc) {
        return init(nullptr, desc);
    }

    bool IntelJpegEncoder::init(ID3D11Device* d3d_device, const JpegEncoderDesc& desc) {
        cleanup();

        if (desc.width == 0 || desc.height == 0 ||
            desc.quality < 1 || desc.quality > 100) {
            std::cerr << "[IntelJpegEncoder] Width, height, and quality must be valid.\n";
            return false;
        }

        desc_ = desc;

        if (d3d_device) {
            d3d_device_ = d3d_device;
            d3d_device_->GetImmediateContext(d3d_context_.GetAddressOf());

            if (open_session(true)) {
                std::cout << "[IntelJpegEncoder] Initialized (D3D11 video memory).\n";
                initialized_ = true;
                return true;
            }

            // Not fatal: the runtime may not accept this device (e.g. it is on a
            // non-Intel adapter). Encoding still works through system memory.
            std::cerr << "[IntelJpegEncoder] Video-memory encoding unavailable, "
                         "falling back to system memory.\n";
            close_session();
        }

        if (!open_session(false)) {
            cleanup();
            return false;
        }

        std::cout << "[IntelJpegEncoder] Initialized (system memory).\n";
        initialized_ = true;
        return true;
    }

    bool IntelJpegEncoder::open_session(bool video_memory) {
        video_memory_ = video_memory;

        // The dispatcher picks the first hardware implementation available
        // (the oneVPL GPU runtime, or the legacy Media SDK runtime on older GPUs).
        loader_ = MFXLoad();
        if (!loader_) {
            std::cerr << "[IntelJpegEncoder] MFXLoad failed.\n";
            return false;
        }

        // The legacy runtime defaults to D3D9; video memory must go through D3D11
        // so it can use our device.
        if (!set_filter_u32(loader_, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE) ||
            (video_memory &&
             !set_filter_u32(loader_, "mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_D3D11)) ||
            !check(MFXCreateSession(loader_, 0, &session_), "MFXCreateSession")) {
            std::cerr << "[IntelJpegEncoder] No Intel hardware runtime found. "
                         "Check that the Intel graphics driver is installed.\n";
            return false;
        }

        if (video_memory) {
            // The runtime submits work on its own threads through this device.
            Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
            if (SUCCEEDED(d3d_device_.As(&multithread))) {
                multithread->SetMultithreadProtected(TRUE);
            }

            allocator_ = {};
            allocator_.pthis = this;
            allocator_.Alloc = &IntelJpegEncoder::alloc_cb;
            allocator_.Lock = &IntelJpegEncoder::lock_cb;
            allocator_.Unlock = &IntelJpegEncoder::unlock_cb;
            allocator_.GetHDL = &IntelJpegEncoder::get_hdl_cb;
            allocator_.Free = &IntelJpegEncoder::free_cb;

            if (!check(MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE, d3d_device_.Get()),
                       "MFXVideoCORE_SetHandle") ||
                !check(MFXVideoCORE_SetFrameAllocator(session_, &allocator_),
                       "MFXVideoCORE_SetFrameAllocator")) {
                return false;
            }
        }

        // JPEG encode from BGRA input. RGB4 input requires 4:4:4 output.
        // Frame dimensions must be 16-aligned; the crop is the real image size.
        mfxVideoParam params{};
        params.mfx.CodecId = MFX_CODEC_JPEG;
        params.mfx.Quality = static_cast<mfxU16>(desc_.quality);
        params.mfx.Interleaved = MFX_SCANTYPE_INTERLEAVED;
        params.mfx.RestartInterval = 0;
        params.mfx.FrameInfo.FourCC = MFX_FOURCC_RGB4;
        params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
        params.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        params.mfx.FrameInfo.FrameRateExtN = 30;
        params.mfx.FrameInfo.FrameRateExtD = 1;
        params.mfx.FrameInfo.CropW = static_cast<mfxU16>(desc_.width);
        params.mfx.FrameInfo.CropH = static_cast<mfxU16>(desc_.height);
        params.mfx.FrameInfo.Width = static_cast<mfxU16>(align16(desc_.width));
        params.mfx.FrameInfo.Height = static_cast<mfxU16>(align16(desc_.height));
        params.IOPattern = video_memory ? MFX_IOPATTERN_IN_VIDEO_MEMORY
                                        : MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        params.AsyncDepth = 1;

        if (!check(MFXVideoENCODE_Init(session_, &params), "MFXVideoENCODE_Init")) {
            return false;
        }

        surface_ = {};
        surface_.Info = params.mfx.FrameInfo;

        const uint32_t frame_bytes =
            static_cast<uint32_t>(params.mfx.FrameInfo.Width) * params.mfx.FrameInfo.Height * 4u;

        if (video_memory) {
            // surface_.Data.MemId is pointed at input_frame_ or external_frame_
            // per frame, in encode_surface()'s callers.
            if (!create_input_texture()) return false;
        } else {
            // System-memory input surface, reused for every frame. RGB4 is stored
            // as packed B, G, R, A bytes, so the channel pointers are offsets into it.
            surface_data_.assign(frame_bytes, 0);
            surface_.Data.B = surface_data_.data();
            surface_.Data.G = surface_.Data.B + 1;
            surface_.Data.R = surface_.Data.B + 2;
            surface_.Data.A = surface_.Data.B + 3;
            set_pitch(surface_.Data, params.mfx.FrameInfo.Width * 4u);
        }

        // A JPEG frame never comes close to the raw BGRA size, so that plus a
        // margin for headers is a safe upper bound for the output buffer.
        mfxVideoParam actual{};
        mfxU32 buffer_size = frame_bytes + 64 * 1024;
        if (MFXVideoENCODE_GetVideoParam(session_, &actual) >= MFX_ERR_NONE) {
            buffer_size = std::max<mfxU32>(buffer_size, actual.mfx.BufferSizeInKB * 1000u);
        }
        bitstream_data_.resize(buffer_size);
        bitstream_ = {};
        bitstream_.Data = bitstream_data_.data();
        bitstream_.MaxLength = buffer_size;

        return true;
    }

    void IntelJpegEncoder::close_session() {
        // MFXClose frees the runtime's own allocations through free_cb, so the
        // allocator state must still be alive here.
        if (session_) {
            MFXVideoENCODE_Close(session_);
            MFXClose(session_);
        }
        if (loader_) MFXUnload(loader_);

        session_ = nullptr;
        loader_ = nullptr;
        allocations_.clear();
        input_frame_ = {};
        external_frame_ = {};
        allocator_ = {};
        surface_ = {};
        bitstream_ = {};
        surface_data_.clear();
        bitstream_data_.clear();
        video_memory_ = false;
    }

    bool IntelJpegEncoder::create_input_texture() {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = surface_.Info.Width;
        desc.Height = surface_.Info.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, input_frame_.texture.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[IntelJpegEncoder] Failed to create input texture.\n";
            return false;
        }
        input_frame_.info = surface_.Info;
        external_frame_.info = surface_.Info;
        return true;
    }

    bool IntelJpegEncoder::encode_bgra(
        const uint8_t* pixels,
        size_t row_pitch,
        std::vector<uint8_t>& jpeg_output) {
        jpeg_output.clear();

        if (!initialized_) {
            std::cerr << "[IntelJpegEncoder] Encoder is not initialized.\n";
            return false;
        }
        if (!pixels || row_pitch < static_cast<size_t>(desc_.width) * 4) {
            std::cerr << "[IntelJpegEncoder] Invalid BGRA input.\n";
            return false;
        }

        if (video_memory_) {
            // Upload into the encoder-owned texture.
            const D3D11_BOX box{0, 0, 0, desc_.width, desc_.height, 1};
            d3d_context_->UpdateSubresource(input_frame_.texture.Get(), 0, &box,
                                            pixels, static_cast<UINT>(row_pitch), 0);
            surface_.Data.MemId = &input_frame_;
            return encode_surface(jpeg_output);
        }

        // Input is already BGRA, which matches RGB4, so this is a row copy only.
        const size_t surface_pitch = static_cast<size_t>(surface_.Info.Width) * 4;
        const size_t row_bytes = static_cast<size_t>(desc_.width) * 4;
        for (uint32_t y = 0; y < desc_.height; ++y) {
            std::memcpy(surface_data_.data() + y * surface_pitch,
                        pixels + y * row_pitch,
                        row_bytes);
        }

        return encode_surface(jpeg_output);
    }

    bool IntelJpegEncoder::encode_texture(
        ID3D11Texture2D* texture,
        std::vector<uint8_t>& jpeg_output) {
        jpeg_output.clear();

        if (!initialized_ || !d3d_context_ || !texture) {
            std::cerr << "[IntelJpegEncoder] Texture encoding is not initialized.\n";
            return false;
        }

        D3D11_TEXTURE2D_DESC src_desc{};
        texture->GetDesc(&src_desc);
        if (src_desc.Width < desc_.width || src_desc.Height < desc_.height ||
            (src_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             src_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            std::cerr << "[IntelJpegEncoder] Texture must be B8G8R8A8 and at least the encoder size.\n";
            return false;
        }

        if (video_memory_) {
            // Zero-copy when the texture already has the layout the runtime
            // expects for an input surface; otherwise a GPU-side copy into the
            // encoder-owned texture. Neither path touches the CPU.
            const bool direct =
                src_desc.Width == surface_.Info.Width &&
                src_desc.Height == surface_.Info.Height &&
                src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                src_desc.ArraySize == 1 &&
                src_desc.SampleDesc.Count == 1 &&
                src_desc.Usage == D3D11_USAGE_DEFAULT &&
                (src_desc.BindFlags & D3D11_BIND_RENDER_TARGET);

            if (direct) {
                external_frame_.texture = texture;
                surface_.Data.MemId = &external_frame_;
            } else {
                const D3D11_BOX box{0, 0, 0, desc_.width, desc_.height, 1};
                d3d_context_->CopySubresourceRegion(input_frame_.texture.Get(), 0, 0, 0, 0,
                                                    texture, 0, &box);
                surface_.Data.MemId = &input_frame_;
            }

            const bool success = encode_surface(jpeg_output);
            // Do not keep the caller's texture alive past this call.
            external_frame_.texture.Reset();
            return success;
        }

        // System-memory fallback: read the texture back through a staging copy.
        // The staging texture is created once, and recreated if the caller
        // switches to a texture with a different description.
        if (!create_staging_texture(src_desc)) return false;

        d3d_context_->CopyResource(staging_texture_.Get(), texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = d3d_context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            std::cerr << "[IntelJpegEncoder] Failed to map staging texture.\n";
            return false;
        }

        const bool success = encode_bgra(
            static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, jpeg_output);

        d3d_context_->Unmap(staging_texture_.Get(), 0);
        return success;
    }

    bool IntelJpegEncoder::create_staging_texture(const D3D11_TEXTURE2D_DESC& src_desc) {
        if (staging_texture_) {
            D3D11_TEXTURE2D_DESC current{};
            staging_texture_->GetDesc(&current);
            if (current.Width == src_desc.Width && current.Height == src_desc.Height &&
                current.Format == src_desc.Format) {
                return true;
            }
            staging_texture_.Reset();
        }

        D3D11_TEXTURE2D_DESC staging_desc{};
        staging_desc.Width = src_desc.Width;
        staging_desc.Height = src_desc.Height;
        staging_desc.MipLevels = 1;
        staging_desc.ArraySize = 1;
        staging_desc.Format = src_desc.Format;
        staging_desc.SampleDesc.Count = 1;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, staging_texture_.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[IntelJpegEncoder] Failed to create staging texture.\n";
            return false;
        }
        return true;
    }

    // Encodes the frame currently in surface_ and copies the JPEG out.
    bool IntelJpegEncoder::encode_surface(std::vector<uint8_t>& jpeg_output) {
        bitstream_.DataOffset = 0;
        bitstream_.DataLength = 0;

        mfxSyncPoint sync_point = nullptr;
        mfxStatus status = MFX_ERR_NONE;
        for (;;) {
            status = MFXVideoENCODE_EncodeFrameAsync(
                session_, nullptr, &surface_, &bitstream_, &sync_point);
            if (status != MFX_WRN_DEVICE_BUSY) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!check(status, "MFXVideoENCODE_EncodeFrameAsync")) return false;
        if (!sync_point) {
            std::cerr << "[IntelJpegEncoder] Encoder produced no output.\n";
            return false;
        }

        // Waiting here also means the runtime is done reading the input texture
        // before control returns to the caller.
        if (!check(MFXVideoCORE_SyncOperation(session_, sync_point, 1000),
                   "MFXVideoCORE_SyncOperation")) {
            return false;
        }

        const uint8_t* data = bitstream_.Data + bitstream_.DataOffset;
        jpeg_output.assign(data, data + bitstream_.DataLength);
        return !jpeg_output.empty();
    }

    // ------------------------------------------------------------------------
    // D3D11 frame allocator (video-memory mode)
    // ------------------------------------------------------------------------

    mfxStatus IntelJpegEncoder::alloc_frames(const mfxFrameAllocRequest& request,
                                             mfxFrameAllocResponse& response) {
        // System-memory requests are served by the runtime itself.
        if (!(request.Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET |
                              MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET))) {
            return MFX_ERR_UNSUPPORTED;
        }

        const DXGI_FORMAT format = to_dxgi_format(request.Info.FourCC);
        if (format == DXGI_FORMAT_UNKNOWN) return MFX_ERR_UNSUPPORTED;

        Allocation allocation;
        for (mfxU16 i = 0; i < request.NumFrameSuggested; ++i) {
            auto frame = std::make_unique<VideoFrame>();
            frame->info = request.Info;
            HRESULT hr;

            if (format == DXGI_FORMAT_P8) {
                // P8 "frames" are linear buffers the encoder writes its bitstream into.
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = static_cast<UINT>(request.Info.Width) * request.Info.Height;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                hr = d3d_device_->CreateBuffer(&desc, nullptr, frame->buffer.GetAddressOf());
            } else {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = request.Info.Width;
                desc.Height = request.Info.Height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = format;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_DECODER;
                if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_YUY2 ||
                    (request.Type & MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET)) {
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                }
                hr = d3d_device_->CreateTexture2D(&desc, nullptr, frame->texture.GetAddressOf());
            }

            if (FAILED(hr)) return MFX_ERR_MEMORY_ALLOC;
            allocation.mids.push_back(frame.get());
            allocation.frames.push_back(std::move(frame));
        }

        allocations_.push_back(std::move(allocation));
        response = {};
        response.mids = allocations_.back().mids.data();
        response.NumFrameActual = request.NumFrameSuggested;
        return MFX_ERR_NONE;
    }

    mfxStatus IntelJpegEncoder::free_frames(mfxFrameAllocResponse& response) {
        auto it = std::find_if(allocations_.begin(), allocations_.end(),
                               [&](const Allocation& a) { return a.mids.data() == response.mids; });
        if (it == allocations_.end()) return MFX_ERR_INVALID_HANDLE;
        allocations_.erase(it);
        response.mids = nullptr;
        response.NumFrameActual = 0;
        return MFX_ERR_NONE;
    }

    mfxStatus IntelJpegEncoder::lock_frame(VideoFrame& frame, mfxFrameData& data) {
        D3D11_MAPPED_SUBRESOURCE mapped{};

        if (frame.buffer) {
            if (FAILED(map_for_read(d3d_context_.Get(), frame.buffer.Get(), mapped))) {
                return MFX_ERR_LOCK_MEMORY;
            }
            data.Y = static_cast<mfxU8*>(mapped.pData);
            data.U = data.V = nullptr;
            set_pitch(data, frame.info.Width);
            return MFX_ERR_NONE;
        }

        if (!frame.texture) return MFX_ERR_LOCK_MEMORY;

        // Textures are read through a CPU-visible staging copy.
        if (!frame.staging) {
            D3D11_TEXTURE2D_DESC desc{};
            frame.texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            if (FAILED(d3d_device_->CreateTexture2D(&desc, nullptr, frame.staging.GetAddressOf()))) {
                return MFX_ERR_LOCK_MEMORY;
            }
        }
        d3d_context_->CopyResource(frame.staging.Get(), frame.texture.Get());
        if (FAILED(map_for_read(d3d_context_.Get(), frame.staging.Get(), mapped))) {
            return MFX_ERR_LOCK_MEMORY;
        }

        auto* base = static_cast<mfxU8*>(mapped.pData);
        set_pitch(data, mapped.RowPitch);
        switch (frame.info.FourCC) {
            case MFX_FOURCC_RGB4:
                data.B = base;
                data.G = base + 1;
                data.R = base + 2;
                data.A = base + 3;
                break;
            case MFX_FOURCC_NV12:
                data.Y = base;
                data.UV = base + static_cast<size_t>(mapped.RowPitch) * frame.info.Height;
                data.V = data.UV + 1;
                break;
            default:
                d3d_context_->Unmap(frame.staging.Get(), 0);
                return MFX_ERR_LOCK_MEMORY;
        }
        return MFX_ERR_NONE;
    }

    mfxStatus IntelJpegEncoder::unlock_frame(VideoFrame& frame, mfxFrameData& data) {
        if (frame.buffer) {
            d3d_context_->Unmap(frame.buffer.Get(), 0);
        } else if (frame.staging) {
            d3d_context_->Unmap(frame.staging.Get(), 0);
        }
        data.Y = data.U = data.V = data.A = nullptr;
        set_pitch(data, 0);
        return MFX_ERR_NONE;
    }

    mfxStatus MFX_CDECL IntelJpegEncoder::alloc_cb(mfxHDL pthis, mfxFrameAllocRequest* request,
                                                   mfxFrameAllocResponse* response) {
        if (!pthis || !request || !response) return MFX_ERR_NULL_PTR;
        return static_cast<IntelJpegEncoder*>(pthis)->alloc_frames(*request, *response);
    }

    mfxStatus MFX_CDECL IntelJpegEncoder::lock_cb(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr) {
        if (!pthis || !mid || !ptr) return MFX_ERR_NULL_PTR;
        return static_cast<IntelJpegEncoder*>(pthis)->lock_frame(*static_cast<VideoFrame*>(mid), *ptr);
    }

    mfxStatus MFX_CDECL IntelJpegEncoder::unlock_cb(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr) {
        if (!pthis || !mid || !ptr) return MFX_ERR_NULL_PTR;
        return static_cast<IntelJpegEncoder*>(pthis)->unlock_frame(*static_cast<VideoFrame*>(mid), *ptr);
    }

    // The runtime asks for the D3D11 resource behind a frame: the texture (or
    // buffer) in .first and the array slice in .second.
    mfxStatus MFX_CDECL IntelJpegEncoder::get_hdl_cb(mfxHDL, mfxMemId mid, mfxHDL* handle) {
        if (!mid || !handle) return MFX_ERR_NULL_PTR;
        const auto* frame = static_cast<VideoFrame*>(mid);
        auto* pair = reinterpret_cast<mfxHDLPair*>(handle);
        pair->first = frame->texture ? static_cast<mfxHDL>(frame->texture.Get())
                                     : static_cast<mfxHDL>(frame->buffer.Get());
        pair->second = nullptr;
        return pair->first ? MFX_ERR_NONE : MFX_ERR_INVALID_HANDLE;
    }

    mfxStatus MFX_CDECL IntelJpegEncoder::free_cb(mfxHDL pthis, mfxFrameAllocResponse* response) {
        if (!pthis || !response) return MFX_ERR_NULL_PTR;
        return static_cast<IntelJpegEncoder*>(pthis)->free_frames(*response);
    }

    void IntelJpegEncoder::cleanup() {
        close_session();
        staging_texture_.Reset();
        d3d_context_.Reset();
        d3d_device_.Reset();
        desc_ = {};
        initialized_ = false;
    }

} // namespace vkit
