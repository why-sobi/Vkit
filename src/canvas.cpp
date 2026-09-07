// src/src/canvas.cpp



#include "canvas.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <climits>

// =========================================================================
// PRIVATE HELPER FUNCTIONS
// =========================================================================

bool DesktopCanvas::create_staging_texture_if_needed(size_t monitor_idx, const D3D11_TEXTURE2D_DESC& gpu_desc) {
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
        std::cerr << "[DesktopCanvas] Failed to create staging texture for monitor " << monitor_idx << "\n";
        return false;
    }

    return true;
}

void DesktopCanvas::copy_bgra_to_master_rgb(const uint8_t* src_pointer, UINT row_pitch, size_t monitor_idx) {
    const auto& meta = monitor_offsets[monitor_idx];
    const size_t master_stride = static_cast<size_t>(master_resolution.first) * 3; // 3 bytes/pixel (RGB)

    for (long y = 0; y < meta.height; ++y) {
        const uint8_t* srcRow = src_pointer + (y * row_pitch);
        
        // Calculate destination offset inside master buffer
        uint8_t* dstRow = master_buffer.data() + 
                            ((meta.y_offset + y) * master_stride) + 
                            (meta.x_offset * 3);

        // Strip Alpha & swap channels: BGRA -> RGB
        for (long x = 0; x < meta.width; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // Red   (from BGRA idx 2)
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // Green (from BGRA idx 1)
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // Blue  (from BGRA idx 0)
        }
    }
}

bool DesktopCanvas::process_single_monitor(size_t monitor_idx) {
    if (this->needs_reinit) { // needed when change on to a full screen mode application like games
        this->init(); // we make it false in the init function
    }

    auto* dupl = desktop_duplications[monitor_idx].Get(); // .Get() returns the pointer to the object 
    if (!dupl) return false;

    ComPtr<IDXGIResource> desktop_resource;

    // Non-blocking wait (0ms timeout) to prevent stalling other active monitors
    HRESULT hr = dupl->AcquireNextFrame(0, &this->frame_info, desktop_resource.GetAddressOf()); // .GetAdderssOf() returns the address of the pointer to the object or aka its reference

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // Screen didn't change on this monitor
    }

    if (FAILED(hr)) {
        // Note: If DXGI_ERROR_ACCESS_LOST occurs (e.g. resolution change), a re-init is required.
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        std::cerr << "[DesktopCanvas] Access lost (app switch/mode change). Re-initialization required.\n";
        // Mark for re-init on next tick
        this->needs_reinit = true; 
        return false;
    }

    // Query D3D11 Texture interface from raw resource
    ComPtr<ID3D11Texture2D> gpu_texture;
    hr = desktop_resource.As(&gpu_texture); // .As is the alternative for the QueryInteface for ComPtr Objects

    if (FAILED(hr) || !gpu_texture) {
        dupl->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC gpu_desc;
    gpu_texture->GetDesc(&gpu_desc);

    // Ensure we have a valid staging texture for this output
    if (!create_staging_texture_if_needed(monitor_idx, gpu_desc)) {
        dupl->ReleaseFrame();
        return false;
    }

    // Copy VRAM GPU texture to VRAM CPU-Staging texture
    d3d11_context->CopyResource(staging_textures[monitor_idx].Get(), gpu_texture.Get());

    // Map CPU Staging Texture to System RAM pointer
    D3D11_MAPPED_SUBRESOURCE mapped_resource;
    hr = d3d11_context->Map(staging_textures[monitor_idx].Get(), 0, D3D11_MAP_READ, 0, &mapped_resource);

    if (SUCCEEDED(hr) && mapped_resource.pData) {
        const uint8_t* src_pointer = static_cast<const uint8_t*>(mapped_resource.pData);

        // Perform direct BGRA -> RGB copy into master buffer
        copy_bgra_to_master_rgb(src_pointer, mapped_resource.RowPitch, monitor_idx);

        d3d11_context->Unmap(staging_textures[monitor_idx].Get(), 0);
        dupl->ReleaseFrame();
        this->initial_frame_captured = true;
        return true;
    }

    dupl->ReleaseFrame();
    return false;
}

// =========================================================================
// PUBLIC MEMBER FUNCTIONS
// =========================================================================

void DesktopCanvas::cleanup() {
    staging_textures.clear();
    desktop_duplications.clear();
    dxgi_outputs.clear();

    d3d11_context.Reset();
    d3d11_device.Reset();
    dxgi_adapter.Reset();
    dxgi_factory.Reset();

    monitor_offsets.clear();
    master_buffer.clear();
    master_resolution = {0, 0};
    initial_frame_captured = false;
    needs_reinit = true;
}

bool DesktopCanvas::init() {
    cleanup();
    this->needs_reinit = false;

    // 1. Create DXGI Factory
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) {
        std::cerr << "[DesktopCanvas] Failed to create DXGI factory." << std::endl;
        return false;
    }

    // 2. Enum Primary Adapter (Main GPU)
    hr = dxgi_factory->EnumAdapters1(0, dxgi_adapter.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[DesktopCanvas] Failed to enumerate primary adapter." << std::endl;
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
        std::cerr << "[DesktopCanvas] No valid monitors detected." << std::endl;
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
        std::cerr << "[DesktopCanvas] Failed to create D3D11 Device." << std::endl;
        return false;
    }

    // 5. Create Duplication Session for EACH Monitor
    for (size_t i = 0; i < dxgi_outputs.size(); ++i) {
        ComPtr<IDXGIOutputDuplication> dupl;
        hr = dxgi_outputs[i]->DuplicateOutput(d3d11_device.Get(), dupl.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[DesktopCanvas] Failed to duplicate monitor index " << i << std::endl;
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

    for (int retry = 0; retry < 5; ++retry) {
        if (capture_frame()) {
            std::cout << "[DesktopCanvas] Initial frame successfully captured on attempt " << (retry + 1) << "\n";
            break;
        }
        Sleep(20); // Give DXGI a moment to fill initial frame buffers
    }

    std::cout << "[DesktopCanvas] Initialized successfully. Master Canvas: "
              << master_resolution.first << "x" << master_resolution.second << " (" << monitor_offsets.size() << " monitors)\n";

    return true;
}

bool DesktopCanvas::capture_frame() {
    if (desktop_duplications.empty()) return false;

    bool updated_any = false;

    for (size_t i = 0; i < desktop_duplications.size(); ++i) {
        if (process_single_monitor(i)) {
            updated_any = true;
        }
    }

    return updated_any;
}

void DesktopCanvas::saveMasterPpm(const char* filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) return;

    int w = master_resolution.first;
    int h = master_resolution.second;

    file << "P6\n" << w << " " << h << "\n255\n";
    file.write(reinterpret_cast<const char*>(master_buffer.data()), master_buffer.size());

    std::cout << "[Saved Master Canvas PPM: " << filename << "]\n";
}