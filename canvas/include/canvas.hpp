// src/include/canvas.hpp
#pragma once

// =========================================================================
// BUILD-TIME CONFIGURATION
//
//   Residence mode (pick at most one, default = RAM):
//     CANVAS_RAM_RESIDENCE   frames are converted to RGB24 in one master buffer in system RAM
//     CANVAS_VRAM_RESIDENCE  frames stay in per-monitor D3D11 textures (get_staging_texture())
//
//   Options:
//     CANVAS_NO_MOUSE_DISPLAY    (opt-out) do not composite the mouse cursor
//
// These macros change the class layout, so they MUST be defined identically in
// every translation unit that includes this header. Set them with the build
// system (e.g. target_compile_definitions(... PUBLIC ...)), not in some .cpp files only.
// =========================================================================
#if defined(CANVAS_RAM_RESIDENCE) && defined(CANVAS_VRAM_RESIDENCE)
    #error "Define only one of CANVAS_RAM_RESIDENCE or CANVAS_VRAM_RESIDENCE."
#endif
#if !defined(CANVAS_RAM_RESIDENCE) && !defined(CANVAS_VRAM_RESIDENCE)
    #define CANVAS_RAM_RESIDENCE            // default to RAM residence
#endif
#ifndef CANVAS_NO_MOUSE_DISPLAY
    #define CANVAS_MOUSE_DISPLAY            // default to mouse display
#endif

#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h> // needed for ComPtr to ensure RAII compliance

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace vkit {

    enum class GpuVendor : UINT {
        NVIDIA = 0x10DE,
        AMD    = 0x1002,
        INTEL  = 0x8086
    };

    // Offset and boundary metadata for each monitor in the Master Canvas
    struct MonitorOffset {
        int id = 0;
        long x_offset = 0;      // Starting X position in master canvas (pixels)
        long y_offset = 0;      // Starting Y position in master canvas (pixels)
        long width = 0;         // Monitor Width (pixels)
        long height = 0;        // Monitor Height (pixels)
    };

#ifdef CANVAS_MOUSE_DISPLAY
    // Latest known cursor state for one monitor. DXGI only reports shape / position when they CHANGE,
    // so Canvas remembers them. In VRAM mode the cursor is NOT composited by Canvas: a downstream GPU
    // pass (e.g. your scaler) reads this and draws it. Re-upload the shape only when shape_serial changes.
    struct PointerState {
        std::vector<uint8_t> shape;                      // raw shape bits from GetFramePointerShape
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};          // Type / Width / Height / Pitch of `shape`
        POINT position{0, 0};                            // top-left of the cursor image, relative to the monitor
        bool visible = false;
        uint64_t shape_serial = 0;                       // bumped every time `shape` / `info` change
    };
#endif

    // ## Image Capture Engine
    // 1. Queries for monitor information, captures the screen into dedicated buffer, and then returns a reference.
    // #### KEYPOINTS
    // 1. Only caters to Windows for now. Linux and MacOS will be added later.
    // 2. This class is not thread-safe. It is expected that the user will call the capture function in a single thread and then use the returned buffer in another thread if needed.
    // 3. This class only returns for the main adapters (i.e output connected to the main GPU).
    // 4. Single buffer for the complete capture even for multiple monitors.
    // 5. capture_frame() transparently re-initialises after DXGI access loss (fullscreen games, resolution
    //    change, UAC prompt...). After that the master buffer may have been reallocated, so re-query
    //    get_master_buffer_ptr() / get_master_resolution() after every capture_frame() call instead of caching them.
    class Canvas {
    private:
        // --- DirectX / DXGI Handles (Zero-Overhead ComPtrs) ---
        ComPtr<IDXGIFactory1> dxgi_factory;     // needed to query adapters
        ComPtr<IDXGIAdapter1> dxgi_adapter;     // needed to query output
        ComPtr<ID3D11Device> d3d11_device;      // the engine that's performing calculations (i.e GPU)
        ComPtr<ID3D11DeviceContext> d3d11_context;

        // --- Per-Monitor DXGI State ---
        std::vector<ComPtr<IDXGIOutput1>> dxgi_outputs;                    // Screens (NOTE: adapters give output that are then converted into output1)
        std::vector<ComPtr<IDXGIOutputDuplication>> desktop_duplications;  // get monitor resolution buffer
        std::vector<ComPtr<ID3D11Texture2D>> frame_textures;               // RAM mode: CPU-readable staging copy. VRAM mode: GPU-only (DEFAULT, SHADER_RESOURCE) copy of the latest frame
        std::vector<uint8_t> monitor_primed;                               // 1 once a monitor has delivered a full frame since the last init()

        // --- Master Canvas Memory & Metadata ---
        std::pair<long, long> master_resolution{0, 0};  // just the size of the buffer in width x height format

        std::vector<MonitorOffset> monitor_offsets;     // where each monitor sits on the virtual desktop / in the master buffer

        // --- Lifetime state ---
        uint64_t generation_ = 0;                       // bumped whenever every D3D object is torn down (cleanup/init)
        bool needs_reinit = true;                       // true until init() succeeds; lets capture_frame() lazily (re)initialise
        ULONGLONG next_init_attempt_ms = 0;             // throttles automatic re-init attempts after a failure

        #ifdef CANVAS_RAM_RESIDENCE
            std::vector<uint8_t> master_buffer;             // Contiguous System RAM in RGB 24-bit format (for all monitors)

            // Converts one BGRA rectangle (monitor-local coordinates) into the master RGB buffer. Clamps to the monitor.
            void copy_rect_to_master_rgb(const uint8_t* src_pointer, UINT row_pitch, size_t monitor_idx, RECT rect);
        #endif // CANVAS_RAM_RESIDENCE

        #ifdef CANVAS_MOUSE_DISPLAY
        std::vector<PointerState> pointers;

            void update_pointer_state(IDXGIOutputDuplication* dupl, size_t monitor_idx, const DXGI_OUTDUPL_FRAME_INFO& fi);

            #ifdef CANVAS_RAM_RESIDENCE
                // Composites the cursor into a pixel surface of size dst_w x dst_h, clipped to that surface.
                // The channel indices say where R/G/B live inside one destination pixel.
                static void blend_pointer(const PointerState& p, uint8_t* dst, size_t dst_pitch,
                                          long dst_w, long dst_h, size_t bytes_per_pixel,
                                          int r_idx, int g_idx, int b_idx);

                void draw_mouse_pointer_cpu(size_t monitor_idx);   // blends into master_buffer
            #endif
        #endif // CANVAS_MOUSE_DISPLAY

        // --- Private Helper Functions ---
        bool create_frame_texture_if_needed(size_t monitor_idx, const D3D11_TEXTURE2D_DESC& gpu_desc);   // we set up the per-monitor frame texture if havent already
        bool process_single_monitor(size_t monitor_idx);                                                 // a helper to be called in a loop inside capture_all_monitors
        bool capture_all_monitors();                                                                     // one pass over every monitor, never re-initialises

    public:
        Canvas() = default;
        ~Canvas() = default; // ComPtr handles RAII cleanup automatically

        // Disable copy constructor and assignment operator (as no copies should be made of this class objects)
        Canvas(const Canvas&) = delete;
        Canvas& operator=(const Canvas&) = delete;

        void cleanup();         // releases every DXGI/D3D object and buffer; the next capture_frame() will re-init
        bool init();            // Sets up factory, adapters, output, duplication devices, buffers and everything before capturing pipeline
        bool capture_frame();   // main function that captures data, converts it and fills up the buffer. True if anything new was captured.

        // --- Getters for nanobind / Python Interface ---
        std::pair<long, long> get_master_resolution() const { return master_resolution; }
        const std::vector<MonitorOffset>& get_monitor_offsets() const { return monitor_offsets; }
        size_t get_monitor_count() const { return desktop_duplications.size(); }

        // Bumped every time init()/cleanup() rebuilds the world. If it differs from the value you saw last
        // time, EVERY pointer/texture/device you fetched from this object is stale and must be re-fetched.
        uint64_t generation() const { return generation_; }

        #ifdef CANVAS_MOUSE_DISPLAY
            const PointerState* get_pointer_state(size_t monitor_idx) const {
                return monitor_idx < pointers.size() ? &pointers[monitor_idx] : nullptr;
            }
        #endif

        #ifdef CANVAS_RAM_RESIDENCE
            const uint8_t* get_master_buffer_ptr() const { return master_buffer.data(); }
            size_t get_master_buffer_size() const { return master_buffer.size(); }

            // Helper to save current Master Buffer to disk for testing (will be removed in the future)
            void saveMasterPpm(const char* filename) const;
        #else
            // GPU-only BGRA copy of the latest frame of one monitor (nullptr until its first frame arrives).
            // Canvas writes it with CopyResource on the immediate context, so a consumer that uses the SAME
            // device/context (get_device()/get_context()) is automatically ordered after the copy.
            ComPtr<ID3D11Texture2D> get_frame_texture(size_t monitor_idx) const;
            ComPtr<ID3D11Device> get_device() const { return d3d11_device; }
            ComPtr<ID3D11DeviceContext> get_context() const { return d3d11_context; }
            // PCI vendor id of the capture adapter; compare with static_cast<UINT>(GpuVendor::NVIDIA) etc.
            UINT get_adapter_vendor_id() const;
        #endif
    };
}