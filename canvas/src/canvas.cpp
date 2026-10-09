// src/src/canvas.cpp
#include "canvas.hpp"

#include <algorithm>
#include <climits>
#include <fstream>
#include <iostream>

#ifdef _MSC_VER
    #pragma comment(lib, "d3d11.lib")
    #pragma comment(lib, "dxgi.lib")
#endif

namespace vkit {

namespace {
    // Guarantees ReleaseFrame() on EVERY exit path after a successful AcquireNextFrame().
    // (A leaked frame makes the next AcquireNextFrame fail with DXGI_ERROR_INVALID_CALL forever.)
    struct FrameReleaser {
        IDXGIOutputDuplication* dupl;
        explicit FrameReleaser(IDXGIOutputDuplication* d) : dupl(d) {}
        ~FrameReleaser() { dupl->ReleaseFrame(); }
        FrameReleaser(const FrameReleaser&) = delete;
        FrameReleaser& operator=(const FrameReleaser&) = delete;
    };
}

// =========================================================================
// PRIVATE HELPER FUNCTIONS
// =========================================================================

bool Canvas::create_frame_texture_if_needed(size_t monitor_idx, const D3D11_TEXTURE2D_DESC& gpu_desc) {
    ComPtr<ID3D11Texture2D>& tex = frame_textures[monitor_idx];

    if (tex) {
        // Re-use only if the desktop texture still has the same geometry/format; otherwise
        // CopyResource would silently fail (mode change that did not raise ACCESS_LOST).
        D3D11_TEXTURE2D_DESC cur{};
        tex->GetDesc(&cur);
        if (cur.Width == gpu_desc.Width && cur.Height == gpu_desc.Height && cur.Format == gpu_desc.Format) {
            return true;
        }
        tex.Reset();
    }

    D3D11_TEXTURE2D_DESC desc = gpu_desc;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
#ifdef CANVAS_RAM_RESIDENCE
    // staging texture is needed for the CPU to access (DXGI makes textures on GPU natively)
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
#else
    // Stays in VRAM: a plain GPU texture that later passes (scaler, encoder interop) can sample.
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
#endif

    HRESULT hr = d3d11_device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[Canvas] Failed to create frame texture for monitor " << monitor_idx << "\n";
        return false;
    }
    return true;
}

#ifdef CANVAS_MOUSE_DISPLAY
void Canvas::update_pointer_state(IDXGIOutputDuplication* dupl, size_t monitor_idx, const DXGI_OUTDUPL_FRAME_INFO& fi) {
    PointerState& p = pointers[monitor_idx];

    // Position/visibility are only meaningful when LastMouseUpdateTime != 0. Reading them on every frame
    // (as before) could make the cursor vanish or jump whenever only the desktop, not the mouse, changed.
    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        p.visible  = (fi.PointerPosition.Visible != FALSE);
        p.position = fi.PointerPosition.Position;
    }

    // A non-zero PointerShapeBufferSize means "the shape changed, here it is".
    if (fi.PointerShapeBufferSize > 0) {
        p.shape.resize(fi.PointerShapeBufferSize);
        UINT required = 0;
        HRESULT hr = dupl->GetFramePointerShape(fi.PointerShapeBufferSize, p.shape.data(), &required, &p.info);
        if (FAILED(hr)) {
            p.shape.clear(); // don't render stale/garbage data
        }
        ++p.shape_serial;
    }
}

#ifdef CANVAS_RAM_RESIDENCE
void Canvas::blend_pointer(const PointerState& p, uint8_t* dst, size_t dst_pitch,
                           long dst_w, long dst_h, size_t bpp,
                           int ri, int gi, int bi) {
    if (!p.visible || p.shape.empty()) return;

    const auto& info = p.info;
    const bool mono   = (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME);
    const bool color  = (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR);
    const bool masked = (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR);
    if (!mono && !color && !masked) return;

    const UINT ptr_w = info.Width;
    const UINT ptr_h = mono ? info.Height / 2 : info.Height; // mono: AND mask on top, XOR mask below

    // Validate the buffer once so the inner loops can't read out of bounds.
    const size_t min_pitch = mono ? (static_cast<size_t>(ptr_w) + 7) / 8 : static_cast<size_t>(ptr_w) * 4;
    if (info.Pitch < min_pitch || p.shape.size() < static_cast<size_t>(info.Pitch) * info.Height) return;

    const uint8_t* and_mask = p.shape.data();
    const uint8_t* xor_mask = and_mask + static_cast<size_t>(info.Pitch) * ptr_h;

    // NOTE: p.position is already the cursor image's top-left corner. The hot-spot is only for
    // hit-testing and must NOT be subtracted (the old code shifted the cursor by its hot-spot).
    for (UINT y = 0; y < ptr_h; ++y) {
        const long ty = p.position.y + static_cast<long>(y);
        if (ty < 0 || ty >= dst_h) continue;

        for (UINT x = 0; x < ptr_w; ++x) {
            const long tx = p.position.x + static_cast<long>(x);
            if (tx < 0 || tx >= dst_w) continue;

            uint8_t* px = dst + static_cast<size_t>(ty) * dst_pitch + static_cast<size_t>(tx) * bpp;

            if (mono) {
                // AND XOR
                //  0   0  -> black      0   1 -> white
                //  1   0  -> unchanged  1   1 -> invert screen
                const size_t idx = static_cast<size_t>(y) * info.Pitch + (x >> 3);
                const uint8_t bit = static_cast<uint8_t>(0x80u >> (x & 7));
                const bool a = (and_mask[idx] & bit) != 0;
                const bool o = (xor_mask[idx] & bit) != 0;

                if (!a) {
                    const uint8_t v = o ? 0xFF : 0x00;
                    px[ri] = v; px[gi] = v; px[bi] = v;
                } else if (o) {
                    px[ri] ^= 0xFF; px[gi] ^= 0xFF; px[bi] ^= 0xFF;
                }
            } else {
                const uint8_t* s = p.shape.data() + static_cast<size_t>(y) * info.Pitch + static_cast<size_t>(x) * 4;
                const uint8_t sb = s[0], sg = s[1], sr = s[2], sa = s[3];

                if (color) {
                    if (sa == 0) continue;                       // fully transparent
                    px[ri] = static_cast<uint8_t>((sr * sa + px[ri] * (255 - sa)) / 255);
                    px[gi] = static_cast<uint8_t>((sg * sa + px[gi] * (255 - sa)) / 255);
                    px[bi] = static_cast<uint8_t>((sb * sa + px[bi] * (255 - sa)) / 255);
                } else {
                    // MASKED_COLOR: the alpha byte is an AND mask, NOT opacity.
                    //   0x00 -> pixel is replaced by the colour,  0xFF -> colour is XORed onto the screen.
                    if (sa == 0) {
                        px[ri] = sr; px[gi] = sg; px[bi] = sb;
                    } else {
                        px[ri] ^= sr; px[gi] ^= sg; px[bi] ^= sb;
                    }
                }
            }
        }
    }
}

#endif // CANVAS_RAM_RESIDENCE (blend_pointer)
#endif // CANVAS_MOUSE_DISPLAY

#ifdef CANVAS_RAM_RESIDENCE
void Canvas::copy_rect_to_master_rgb(const uint8_t* src_pointer, UINT row_pitch, size_t monitor_idx, RECT rect) {
    const auto& meta = monitor_offsets[monitor_idx];

    // Clamp to the monitor so a bad rect can never read/write out of bounds.
    rect.left   = (std::max)(rect.left, 0L);
    rect.top    = (std::max)(rect.top, 0L);
    rect.right  = (std::min)(rect.right, meta.width);
    rect.bottom = (std::min)(rect.bottom, meta.height);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;

    const size_t master_stride = static_cast<size_t>(master_resolution.first) * 3; // 3 bytes/pixel (RGB)
    const size_t rect_w = static_cast<size_t>(rect.right - rect.left);

    for (long y = rect.top; y < rect.bottom; ++y) {
        const uint8_t* srcRow = src_pointer + static_cast<size_t>(y) * row_pitch + static_cast<size_t>(rect.left) * 4;
        uint8_t* dstRow = master_buffer.data()
                        + static_cast<size_t>(meta.y_offset + y) * master_stride
                        + static_cast<size_t>(meta.x_offset + rect.left) * 3;

        for (size_t x = 0; x < rect_w; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // Red   (from BGRA idx 2)
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // Green (from BGRA idx 1)
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // Blue  (from BGRA idx 0)
        }
    }
}

void Canvas::saveMasterPpm(const char* filename) const {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[Canvas] Could not open " << filename << " for writing\n";
        return;
    }

    file << "P6\n" << master_resolution.first << " " << master_resolution.second << "\n255\n";
    file.write(reinterpret_cast<const char*>(master_buffer.data()), static_cast<std::streamsize>(master_buffer.size()));

    std::cout << "[Saved Master Canvas PPM: " << filename << "]\n";
}

#ifdef CANVAS_MOUSE_DISPLAY
void Canvas::draw_mouse_pointer_cpu(size_t monitor_idx) {
    const auto& meta = monitor_offsets[monitor_idx];
    const size_t master_stride = static_cast<size_t>(master_resolution.first) * 3;

    uint8_t* monitor_origin = master_buffer.data()
                            + static_cast<size_t>(meta.y_offset) * master_stride
                            + static_cast<size_t>(meta.x_offset) * 3;

    // Clipped to THIS monitor (not the whole canvas) so the cursor can't bleed into a neighbour's region.
    blend_pointer(pointers[monitor_idx], monitor_origin, master_stride, meta.width, meta.height,
                  /*bytes_per_pixel*/ 3, /*r*/ 0, /*g*/ 1, /*b*/ 2);
}
#endif // CANVAS_MOUSE_DISPLAY

#else // CANVAS_VRAM_RESIDENCE

ComPtr<ID3D11Texture2D> Canvas::get_frame_texture(size_t monitor_idx) const {
    if (monitor_idx >= frame_textures.size()) return nullptr;
    return frame_textures[monitor_idx];
}

UINT Canvas::get_adapter_vendor_id() const {
    if (!dxgi_adapter) return 0;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(dxgi_adapter->GetDesc1(&desc))) return 0;
    return desc.VendorId;
}
#endif // CANVAS_RAM_RESIDENCE

bool Canvas::process_single_monitor(size_t monitor_idx) {
    IDXGIOutputDuplication* dupl = desktop_duplications[monitor_idx].Get();
    if (!dupl) return false;

    // 1. Acquire frame from DXGI
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> desktop_resource;
    HRESULT hr = dupl->AcquireNextFrame(0, &frame_info, desktop_resource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { return false; }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        std::cerr << "[Canvas] Access lost. Re-initialization required.\n";
        this->needs_reinit = true;
        return false;
    }
    if (FAILED(hr)) { return false; }

    FrameReleaser frame_guard(dupl); // from here on, every return releases the frame

#ifdef CANVAS_MOUSE_DISPLAY
    // 1b. Track cursor position / shape (must happen while the frame is held)
    update_pointer_state(dupl, monitor_idx, frame_info);
#endif

    // 2. Extract GPU texture
    ComPtr<ID3D11Texture2D> gpu_texture;
    if (FAILED(desktop_resource.As(&gpu_texture)) || !gpu_texture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC gpu_desc{};
    gpu_texture->GetDesc(&gpu_desc);

    if (!create_frame_texture_if_needed(monitor_idx, gpu_desc)) {
        return false;
    }
    ID3D11Texture2D* staging = frame_textures[monitor_idx].Get();

    // 3. Copy VRAM Texture -> CPU-Staging Texture
    d3d11_context->CopyResource(staging, gpu_texture.Get());

#ifdef CANVAS_RAM_RESIDENCE
    // 4. Map Staging Texture & Copy to Master Buffer
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = d3d11_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        if (FAILED(d3d11_device->GetDeviceRemovedReason())) this->needs_reinit = true;
        return false;
    }

    const uint8_t* src_pointer = static_cast<const uint8_t*>(mapped.pData);
    const auto& meta = monitor_offsets[monitor_idx];
    const RECT whole_monitor{0, 0, meta.width, meta.height};

    copy_rect_to_master_rgb(src_pointer, mapped.RowPitch, monitor_idx, whole_monitor);

    #ifdef CANVAS_MOUSE_DISPLAY
        draw_mouse_pointer_cpu(monitor_idx);
    #endif

    d3d11_context->Unmap(staging, 0);
    monitor_primed[monitor_idx] = 1;
    return true;

#else // CANVAS_VRAM_RESIDENCE
    // Frame is already in frame_textures[monitor_idx]; the cursor is left to the downstream GPU pass
    // (see get_pointer_state()).
    monitor_primed[monitor_idx] = 1;
    return true;
#endif
}

// =========================================================================
// PUBLIC MEMBER FUNCTIONS
// =========================================================================

void Canvas::cleanup() {
    ++generation_; // every handle previously handed out is about to become invalid
    frame_textures.clear();
    desktop_duplications.clear();
    dxgi_outputs.clear();
    monitor_primed.clear();
    monitor_offsets.clear();

    d3d11_context.Reset();
    d3d11_device.Reset();
    dxgi_adapter.Reset();
    dxgi_factory.Reset();

#ifdef CANVAS_MOUSE_DISPLAY
    pointers.clear();
#endif
#ifdef CANVAS_RAM_RESIDENCE
    master_buffer.clear();
#endif
    master_resolution = {0, 0};
    needs_reinit = true;
}

bool Canvas::init() {
    cleanup();
    this->needs_reinit = false;

    // Any failure below tears everything down again. That leaves needs_reinit == true so
    // capture_frame() retries later, instead of limping on with half-built state.
    auto fail = [this](const char* msg) {
        std::cerr << "[Canvas] " << msg << std::endl;
        cleanup();
        return false;
    };

    // 1. Create DXGI Factory
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) return fail("Failed to create DXGI factory.");

    // 2. Enum Primary Adapter (Main GPU)
    hr = dxgi_factory->EnumAdapters1(0, dxgi_adapter.GetAddressOf());
    if (FAILED(hr)) return fail("Failed to enumerate primary adapter.");

    // 3. Enum Outputs (Monitors) - skip outputs that aren't part of the desktop
    std::vector<DXGI_OUTPUT_DESC> output_descs;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIOutput> temp_out;
        if (FAILED(dxgi_adapter->EnumOutputs(i, temp_out.GetAddressOf()))) break;

        ComPtr<IDXGIOutput1> out1;
        if (FAILED(temp_out.As(&out1))) continue;

        DXGI_OUTPUT_DESC od{};
        if (FAILED(out1->GetDesc(&od)) || !od.AttachedToDesktop) continue;

        dxgi_outputs.push_back(out1);
        output_descs.push_back(od);
    }

    if (dxgi_outputs.empty()) return fail("No valid monitors detected.");

    // 4. Create D3D11 Device & Context
    hr = D3D11CreateDevice(
        dxgi_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, // needed by Direct2D/video-processor interop on the same device
        nullptr,
        0,
        D3D11_SDK_VERSION,
        d3d11_device.GetAddressOf(),
        nullptr,
        d3d11_context.GetAddressOf()
    );
    if (FAILED(hr)) return fail("Failed to create D3D11 Device.");

    // 5. Create Duplication Session for EACH Monitor
    for (size_t i = 0; i < dxgi_outputs.size(); ++i) {
        ComPtr<IDXGIOutputDuplication> dupl;
        hr = dxgi_outputs[i]->DuplicateOutput(d3d11_device.Get(), dupl.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "[Canvas] Failed to duplicate monitor index " << i
                      << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << ")\n";
            cleanup();
            return false;
        }
        desktop_duplications.push_back(dupl);
    }

    frame_textures.resize(desktop_duplications.size());
    monitor_primed.assign(desktop_duplications.size(), 0);
#ifdef CANVAS_MOUSE_DISPLAY
    pointers.assign(desktop_duplications.size(), PointerState{});
#endif

    // 6. Calculate Bounding Box and Offsets
    long min_x = LONG_MAX, min_y = LONG_MAX;
    long max_x = LONG_MIN, max_y = LONG_MIN;

    for (const auto& od : output_descs) {
        min_x = (std::min)(min_x, od.DesktopCoordinates.left);
        min_y = (std::min)(min_y, od.DesktopCoordinates.top);
        max_x = (std::max)(max_x, od.DesktopCoordinates.right);
        max_y = (std::max)(max_y, od.DesktopCoordinates.bottom);
    }

    master_resolution = { max_x - min_x, max_y - min_y };

    for (size_t i = 0; i < output_descs.size(); ++i) {
        const RECT& rc = output_descs[i].DesktopCoordinates;

        MonitorOffset meta;
        meta.id = static_cast<int>(i);
        meta.width = rc.right - rc.left;
        meta.height = rc.bottom - rc.top;
        meta.x_offset = rc.left - min_x;
        meta.y_offset = rc.top - min_y;

        // The duplicated texture is what we actually read from. If it is smaller than the desktop rect
        // (rotated output) we crop to it rather than reading past the end of the mapped texture.
        DXGI_OUTDUPL_DESC dd{};
        desktop_duplications[i]->GetDesc(&dd);
        if (static_cast<long>(dd.ModeDesc.Width) != meta.width || static_cast<long>(dd.ModeDesc.Height) != meta.height) {
            std::cerr << "[Canvas] Warning: monitor " << i << " duplicates at " << dd.ModeDesc.Width << "x" << dd.ModeDesc.Height
                      << " but occupies " << meta.width << "x" << meta.height
                      << " on the desktop (rotated output?). Cropping; rotation is not handled.\n";
            meta.width  = (std::min)(meta.width,  static_cast<long>(dd.ModeDesc.Width));
            meta.height = (std::min)(meta.height, static_cast<long>(dd.ModeDesc.Height));
        }
        if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            std::cerr << "[Canvas] Warning: monitor " << i << " is not B8G8R8A8 (HDR / wide-gamut?); "
                         "the BGRA->RGB conversion will produce wrong colours.\n";
        }

        monitor_offsets.push_back(meta);
    }

#ifdef CANVAS_RAM_RESIDENCE
    // Allocate Master Buffer (RGB 24-bit)
    size_t master_bytes = static_cast<size_t>(master_resolution.first) * static_cast<size_t>(master_resolution.second) * 3;
    master_buffer.assign(master_bytes, 0);
#endif

    // 7. Warm-up: wait until EVERY monitor has delivered a first frame. (Stopping at the first success, as
    //    before, left a static second monitor black until something on it changed.)
    //    capture_all_monitors() is used instead of capture_frame() so a failure here can't recurse into init().
    bool all_primed = false;
    for (int attempt = 0; attempt < 10 && !all_primed; ++attempt) {
        capture_all_monitors();
        all_primed = std::all_of(monitor_primed.begin(), monitor_primed.end(), [](uint8_t v) { return v != 0; });
        if (!all_primed) Sleep(20);
    }
    if (!all_primed) {
        std::cerr << "[Canvas] Warning: not every monitor produced an initial frame yet.\n";
    }

    std::cout << "[Canvas] Initialized successfully. Master Canvas: "
              << master_resolution.first << "x" << master_resolution.second
              << " (" << dxgi_outputs.size() << " monitors)\n";

    return true;
}

bool Canvas::capture_all_monitors() {
    bool updated_any = false;
    for (size_t i = 0; i < desktop_duplications.size(); ++i) {
        if (process_single_monitor(i)) {
            updated_any = true;
        }
    }
    return updated_any;
}

bool Canvas::capture_frame() {
    bool reinitialized = false;

    if (needs_reinit) { // set after ACCESS_LOST (fullscreen game, resolution change, UAC...) or a failed init
        // Don't hammer DuplicateOutput (and stderr) every frame while it keeps failing.
        if (GetTickCount64() < next_init_attempt_ms) return false;

        if (!init()) {
            next_init_attempt_ms = GetTickCount64() + 500;
            return false;
        }
        reinitialized = true; // buffer contents / size changed, so callers should treat this as a new frame
    }

    const bool updated = capture_all_monitors();
    return updated || reinitialized;
}

} // namespace vkit