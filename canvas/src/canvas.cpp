// src/src/canvas.cpp
#include "canvas.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <climits>

// =========================================================================
// PRIVATE HELPER FUNCTIONS
// =========================================================================
namespace vkit {
    bool Canvas::create_staging_texture_if_needed(size_t monitor_idx, const D3D11_TEXTURE2D_DESC& gpu_desc) {
        if (staging_textures[monitor_idx] != nullptr) {
            return true; // Already initialized
        }

        D3D11_TEXTURE2D_DESC staging_desc = gpu_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.BindFlags = 0;
        staging_desc.MiscFlags = 0;
        staging_desc.MipLevels = 1;
        staging_desc.ArraySize = 1;
        staging_desc.SampleDesc.Count = 1;

        HRESULT hr = d3d11_device->CreateTexture2D(&staging_desc, nullptr, &staging_textures[monitor_idx]); // staging texture is needed for the CPU to access (DXGI makes textures on GPU natively)
        if (FAILED(hr)) {
            std::cerr << "[Canvas] Failed to create staging texture for monitor " << monitor_idx << "\n";
            return false;
        }

        return true;
    }

#ifdef CANVAS_RAM_RESIDENCE
    void Canvas::copy_bgra_to_master_rgb(const uint8_t* src_pointer, UINT row_pitch, size_t monitor_idx) {
        const auto& meta = monitor_offsets[monitor_idx];
        const size_t master_stride = static_cast<size_t>(master_resolution.first) * 3; // 3 bytes/pixel (RGB)

        for (long y = 0; y < meta.height; ++y) {
            const uint8_t* srcRow = src_pointer + (y * row_pitch);
            
            uint8_t* dstRow = master_buffer.data() + 
                                ((meta.y_offset + y) * master_stride) + 
                                (meta.x_offset * 3);

            for (long x = 0; x < meta.width; ++x) {
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // Red   (from BGRA idx 2)
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // Green (from BGRA idx 1)
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // Blue  (from BGRA idx 0)
            }
        }
    }
    void Canvas::saveMasterPpm(const char* filename) {
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) return;

        int w = master_resolution.first;
        int h = master_resolution.second;

        file << "P6\n" << w << " " << h << "\n255\n";
        file.write(reinterpret_cast<const char*>(master_buffer.data()), master_buffer.size());

        std::cout << "[Saved Master Canvas PPM: " << filename << "]\n";
    }

    #ifdef CANVAS_DIRTY_RECT_OPT
        void Canvas::copy_dirty_rects_to_master_rgb(const uint8_t* src_pointer, 
                                                    UINT row_pitch, 
                                                    size_t monitor_idx, 
                                                    const std::vector<RECT>& dirty_rects) {
            const auto& meta = monitor_offsets[monitor_idx];
            const size_t master_stride = static_cast<size_t>(master_resolution.first) * 3;

            for (const auto& rect : dirty_rects) {
                long rect_w = rect.right - rect.left;
                long rect_h = rect.bottom - rect.top;

                for (long y = 0; y < rect_h; ++y) {
                    long src_y = rect.top + y;
                    long dst_y = meta.y_offset + src_y;

                    const uint8_t* srcRow = src_pointer + (src_y * row_pitch) + (rect.left * 4);
                    uint8_t* dstRow = master_buffer.data() + (dst_y * master_stride) + ((meta.x_offset + rect.left) * 3);

                    for (long x = 0; x < rect_w; ++x) {
                        dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // Red (from BGRA index 2)
                        dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // Green (from BGRA index 1)
                        dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // Blue (from BGRA index 0)
                    }
                }
            }
        }
    #endif

#else // CANVAS_VRAM_RESIDENCE
    ComPtr<ID3D11Texture2D> Canvas::get_staging_texture(size_t monitor_idx) const { 
        if (monitor_idx >= staging_textures.size()) return nullptr;
        return staging_textures[monitor_idx]; 
    }
    ComPtr<ID3D11Texture2D> Canvas::get_staging_texture(GpuVendor vendor) const { 
        for (size_t i = 0; i < staging_textures.size(); ++i) {
            ComPtr<ID3D11Device> device;
            staging_textures[i]->GetDevice(device.GetAddressOf());
            if (!device) continue;

            ComPtr<IDXGIDevice> dxgi_dev;
            if (FAILED(device.As(&dxgi_dev))) continue;

            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(dxgi_dev->GetAdapter(adapter.GetAddressOf()))) continue;

            DXGI_ADAPTER_DESC desc;
            adapter->GetDesc(&desc);
            if (desc.VendorId == static_cast<UINT>(vendor)) {
                return staging_textures[i];
            }
        }
        return nullptr; 
    }
#endif // CANVAS_RAM_RESIDENCE

    bool Canvas::process_single_monitor(size_t monitor_idx) {
        auto* dupl = desktop_duplications[monitor_idx].Get();
        if (!dupl) return false;

        ComPtr<IDXGIResource> desktop_resource;

        // 1. Acquire frame from DXGI
        HRESULT hr = dupl->AcquireNextFrame(0, &this->frame_info, desktop_resource.GetAddressOf());

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { return false; } 
        if (hr == DXGI_ERROR_ACCESS_LOST) { 
            std::cerr << "[Canvas] Access lost. Re-initialization required.\n";
            this->needs_reinit = true; 
            return false;
        }
        if (FAILED(hr)) { return false; }  

        // 2. Extract GPU texture
        ComPtr<ID3D11Texture2D> gpu_texture;
        hr = desktop_resource.As(&gpu_texture);

        if (FAILED(hr) || !gpu_texture) {
            dupl->ReleaseFrame();
            return false;
        }

        D3D11_TEXTURE2D_DESC gpu_desc;
        gpu_texture->GetDesc(&gpu_desc);

        if (!create_staging_texture_if_needed(monitor_idx, gpu_desc)) {
            dupl->ReleaseFrame();
            return false;
        }

        // 3. Query Dirty Rectangles from DXGI
    #ifdef CANVAS_DIRTY_RECT_OPT
        std::vector<RECT> dirty_rects;
        UINT dirty_rects_buffer_size = frame_info.TotalMetadataBufferSize;

        if (dirty_rects_buffer_size > 0) {
            UINT buf_size_needed = 0;
            dirty_rects.resize(dirty_rects_buffer_size / sizeof(RECT));
            
            hr = dupl->GetFrameDirtyRects(
                dirty_rects_buffer_size, 
                dirty_rects.data(), 
                &buf_size_needed
            );

            if (FAILED(hr)) {
                dirty_rects.clear();
            } else {
                dirty_rects.resize(buf_size_needed / sizeof(RECT));
            }
        }
    #endif

        // 4. Copy VRAM Texture -> CPU-Staging Texture
        d3d11_context->CopyResource(staging_textures[monitor_idx].Get(), gpu_texture.Get());

    #ifdef CANVAS_RAM_RESIDENCE
        // 5. Map Staging Texture & Copy to Master Buffer
        D3D11_MAPPED_SUBRESOURCE mapped_resource;
        hr = d3d11_context->Map(staging_textures[monitor_idx].Get(), 0, D3D11_MAP_READ, 0, &mapped_resource);

        if (SUCCEEDED(hr) && mapped_resource.pData) {
            const uint8_t* src_pointer = static_cast<const uint8_t*>(mapped_resource.pData);

        #ifdef CANVAS_DIRTY_RECT_OPT
                // If it's the initial frame or dirty_rects is empty, do a full copy. Otherwise, copy only dirty rects.
                if (!this->initial_frame_captured || dirty_rects.empty()) {
                    copy_bgra_to_master_rgb(src_pointer, mapped_resource.RowPitch, monitor_idx);
                } else {
                    copy_dirty_rects_to_master_rgb(src_pointer, mapped_resource.RowPitch, monitor_idx, dirty_rects);
                }
        #else
                // Standard full-frame copy
                copy_bgra_to_master_rgb(src_pointer, mapped_resource.RowPitch, monitor_idx);
        #endif

        d3d11_context->Unmap(staging_textures[monitor_idx].Get(), 0);
        dupl->ReleaseFrame();
        this->initial_frame_captured = true;
        return true;
    }
    #else
        // VRAM Residence Mode
        dupl->ReleaseFrame();
        this->initial_frame_captured = true;
        return true;
    #endif

        dupl->ReleaseFrame();
        return false;
    }

    // =========================================================================
    // PUBLIC MEMBER FUNCTIONS
    // =========================================================================

    void Canvas::cleanup() {
            staging_textures.clear();
            desktop_duplications.clear();
            dxgi_outputs.clear();

            d3d11_context.Reset();
            d3d11_device.Reset();
            dxgi_adapter.Reset();
            dxgi_factory.Reset();

    #ifdef CANVAS_RAM_RESIDENCE
            monitor_offsets.clear();
            master_buffer.clear();
    #endif
            master_resolution = {0, 0};
            initial_frame_captured = false;
            needs_reinit = true;
        }

    bool Canvas::init() {
        cleanup();
        this->needs_reinit = false;

        // 1. Create DXGI Factory
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
        if (FAILED(hr)) {
            std::cerr << "[Canvas] Failed to create DXGI factory." << std::endl;
            return false;
        }

        // 2. Enum Primary Adapter (Main GPU)
        hr = dxgi_factory->EnumAdapters1(0, dxgi_adapter.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[Canvas] Failed to enumerate primary adapter." << std::endl;
            return false;
        }

        // 3. Enum Outputs (Monitors)
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> temp_out;
            if (FAILED(dxgi_adapter->EnumOutputs(i, temp_out.GetAddressOf()))) break;

            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(temp_out.As(&out1))) {
                dxgi_outputs.push_back(out1);
            }
        }

        if (dxgi_outputs.empty()) {
            std::cerr << "[Canvas] No valid monitors detected." << std::endl;
            return false;
        }

        // 4. Create D3D11 Device & Context
        hr = D3D11CreateDevice(
            dxgi_adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            d3d11_device.GetAddressOf(),
            nullptr,
            d3d11_context.GetAddressOf()
        );
        if (FAILED(hr)) {
            std::cerr << "[Canvas] Failed to create D3D11 Device." << std::endl;
            return false;
        }

        // 5. Create Duplication Session for EACH Monitor
        for (size_t i = 0; i < dxgi_outputs.size(); ++i) {
            ComPtr<IDXGIOutputDuplication> dupl;
            hr = dxgi_outputs[i]->DuplicateOutput(d3d11_device.Get(), dupl.GetAddressOf());
            if (FAILED(hr)) {
                std::cerr << "[Canvas] Failed to duplicate monitor index " << i << std::endl;
                return false;
            }
            desktop_duplications.push_back(dupl);
        }

        staging_textures.resize(desktop_duplications.size());

        // 6. Calculate Bounding Box and Offsets
        long min_x = LONG_MAX, min_y = LONG_MAX;
        long max_x = LONG_MIN, max_y = LONG_MIN;

        for (const auto& output : dxgi_outputs) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            min_x = (std::min)(min_x, desc.DesktopCoordinates.left);
            min_y = (std::min)(min_y, desc.DesktopCoordinates.top);
            max_x = (std::max)(max_x, desc.DesktopCoordinates.right);
            max_y = (std::max)(max_y, desc.DesktopCoordinates.bottom);
        }

        master_resolution = { max_x - min_x, max_y - min_y };

#ifdef CANVAS_RAM_RESIDENCE
        for (size_t i = 0; i < dxgi_outputs.size(); ++i) {
            DXGI_OUTPUT_DESC desc;
            dxgi_outputs[i]->GetDesc(&desc);

            MonitorOffset meta;
            meta.id = static_cast<int>(i);
            meta.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            meta.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            meta.x_offset = desc.DesktopCoordinates.left - min_x;
            meta.y_offset = desc.DesktopCoordinates.top - min_y;

            monitor_offsets.push_back(meta);
        }

        // Allocate Master Buffer (RGB 24-bit)
        size_t master_bytes = static_cast<size_t>(master_resolution.first) * static_cast<size_t>(master_resolution.second) * 3;
        master_buffer.assign(master_bytes, 0);
#endif

        for (int retry = 0; retry < 5; ++retry) {
            if (capture_frame()) {
                std::cout << "[Canvas] Initial frame successfully captured on attempt " << (retry + 1) << "\n";
                break;
            }
            Sleep(20);
        }

        std::cout << "[Canvas] Initialized successfully. Master Canvas: "
                  << master_resolution.first << "x" << master_resolution.second << " (" << dxgi_outputs.size() << " monitors)\n";

        return true;
    
    }

    bool Canvas::capture_frame() {
        if (desktop_duplications.empty()) return false;
        if (this->needs_reinit) { // needed when change on to a full screen mode application like games
            this->init(); // we make it false in the init function
            
        }

        bool updated_any = false;

        for (size_t i = 0; i < desktop_duplications.size(); ++i) {
            if (process_single_monitor(i)) {
                updated_any = true;
            }
        }

        return updated_any;
    }

} // namespace vkit