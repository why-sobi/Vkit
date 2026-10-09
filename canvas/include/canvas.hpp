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

#ifdef CANVAS_VRAM_RESIDENCE
    enum class GpuVendor : UINT {
        NVIDIA = 0x10DE,
        AMD    = 0x1002,
        INTEL  = 0x8086
    };
#endif // CANVAS_VRAM_RESIDENCE

    // Offset and boundary metadata for each monitor in the Master Canvas
    struct MonitorOffset {
        int id = 0;
        long x_offset = 0;      // Starting X position in master canvas (pixels)
        long y_offset = 0;      // Starting Y position in master canvas (pixels)
        long width = 0;         // Monitor Width (pixels)
        long height = 0;        // Monitor Height (pixels)
    };

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
        std::vector<ComPtr<ID3D11Texture2D>> staging_textures;             // this is where we move screen image from VRAM to RAM
        std::vector<uint8_t> monitor_primed;                               // 1 once a monitor has delivered a full frame since the last init()

        // --- Master Canvas Memory & Metadata ---
        std::pair<long, long> master_resolution{0, 0};  // just the size of the buffer in width x height format

        // --- Lifetime state ---
        bool needs_reinit = true;                       // true until init() succeeds; lets capture_frame() lazily (re)initialise
        ULONGLONG next_init_attempt_ms = 0;             // throttles automatic re-init attempts after a failure

        #ifdef CANVAS_RAM_RESIDENCE
            std::vector<uint8_t> master_buffer;             // Contiguous System RAM in RGB 24-bit format (for all monitors)
            std::vector<MonitorOffset> monitor_offsets;     // tells where the offsets of monitor are (with other meta data) in the master buffer

            // Converts one BGRA rectangle (monitor-local coordinates) into the master RGB buffer. Clamps to the monitor.
            void copy_rect_to_master_rgb(const uint8_t* src_pointer, UINT row_pitch, size_t monitor_idx, RECT rect);
        #endif // CANVAS_RAM_RESIDENCE

        #ifdef CANVAS_MOUSE_DISPLAY
            // DXGI only reports shape / position when they CHANGE, so we must remember them per monitor.
            struct PointerState {
                std::vector<uint8_t> shape;                      // raw shape bits from GetFramePointerShape
                DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
                POINT position{0, 0};                            // top-left of the cursor image, relative to the monitor
                bool visible = false;
            };
            std::vector<PointerState> pointers;

            void update_pointer_state(IDXGIOutputDuplication* dupl, size_t monitor_idx, const DXGI_OUTDUPL_FRAME_INFO& fi);

            // Composites the cursor into a pixel surface of size dst_w x dst_h, clipped to that surface.
            // The channel indices say where R/G/B live inside one destination pixel.
            static void blend_pointer(const PointerState& p, uint8_t* dst, size_t dst_pitch,
                                      long dst_w, long dst_h, size_t bytes_per_pixel,
                                      int r_idx, int g_idx, int b_idx);

            #ifdef CANVAS_RAM_RESIDENCE
                void draw_mouse_pointer_cpu(size_t monitor_idx);   // blends into master_buffer
            #else
                void draw_mouse_pointer_gpu(size_t monitor_idx);   // blends into the (CPU-visible) staging texture
            #endif
        #endif // CANVAS_MOUSE_DISPLAY

        // --- Private Helper Functions ---
        bool create_staging_texture_if_needed(size_t monitor_idx, const D3D11_TEXTURE2D_DESC& gpu_desc); // we set up staging texture if havent already
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

        #ifdef CANVAS_RAM_RESIDENCE
            const uint8_t* get_master_buffer_ptr() const { return master_buffer.data(); }
            size_t get_master_buffer_size() const { return master_buffer.size(); }
            const std::vector<MonitorOffset>& get_monitor_offsets() const { return monitor_offsets; }

            // Helper to save current Master Buffer to disk for testing (will be removed in the future)
            void saveMasterPpm(const char* filename) const;
        #else
            ComPtr<ID3D11Texture2D> get_staging_texture(size_t monitor_idx) const;   // staging texture for a specific monitor index
            ComPtr<ID3D11Texture2D> get_staging_texture(GpuVendor vendor) const;     // staging texture if the capture adapter is from this vendor (NVIDIA, AMD, INTEL)
        #endif
    };
}