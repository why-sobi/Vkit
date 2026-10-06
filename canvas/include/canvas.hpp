// src/inlude/canvas.hpp
#pragma once

#ifndef CANVAS_VRAM_RESIDENCE
    #define CANVAS_RAM_RESIDENCE // default to RAM residence if not defined
#endif
#ifndef CANVAS_DIRTY_RECT_OPT
    #define CANVAS_NO_DIRTY_RECT_OPT // default to no dirty rectangle optimization if not defined
#endif

#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h> // needed for ComPtr to ensure RAII compliance

#include <vector>
#include <utility>
#include <cstdint>

using Microsoft::WRL::ComPtr;


// Offset and boundary metadata for each monitor in the Master Canvas
namespace vkit {
#ifdef CANVAS_VRAM_RESIDENCE
    enum class GpuVendor : UINT {
        NVIDIA = 0x10DE,
        AMD    = 0x1002,
        INTEL  = 0x8086
    };
#endif // CANVAS_VRAM_RESIDENCE

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
    // 4. Single buffer for the complete capture even for multiple monitors
    class Canvas {
    private:
        // --- DirectX / DXGI Handles (Zero-Overhead ComPtrs) ---
        ComPtr<IDXGIFactory1> dxgi_factory; // needed to query adapters
        ComPtr<IDXGIAdapter1> dxgi_adapter; // needed to query output
        ComPtr<ID3D11Device> d3d11_device;  // the engine that's performing calculations (i.e GPU) 
        ComPtr<ID3D11DeviceContext> d3d11_context; 

        // --- Per-Monitor DXGI State ---
        std::vector<ComPtr<IDXGIOutput1>> dxgi_outputs; // Screens (NOTE: adapters give output that are then converted into output1)
        std::vector<ComPtr<IDXGIOutputDuplication>> desktop_duplications; // get monitor resolution buffer
        std::vector<ComPtr<ID3D11Texture2D>> staging_textures; // this is where we move screen image from VRAM to RAM

        // --- Master Canvas Memory & Metadata ---
        std::pair<long, long> master_resolution{0, 0};  // just the size of the buffer in width x height format

        #ifdef CANVAS_RAM_RESIDENCE
            std::vector<uint8_t> master_buffer;             // Contiguous System RAM in RGB 24-bit format (for all monitors)
            std::vector<MonitorOffset> monitor_offsets;     // tells where the offsets of monitor are (with other meta data) in the master buffer

            #ifdef CANVAS_DIRTY_RECT_OPT
            void copy_dirty_rects_to_master_rgb(const uint8_t* src_pointer, 
                                                UINT row_pitch, 
                                                size_t monitor_idx, 
                                                const std::vector<RECT>& dirty_rects);
            #endif // CANVAS_DIRTY_RECT_OPT
        
        #endif // CANVAS_RAM_RESIDENCE

        DXGI_OUTDUPL_FRAME_INFO frame_info{};    
        bool initial_frame_captured; 
        bool needs_reinit;      

        // --- Private Helper Functions ---
        bool create_staging_texture_if_needed(size_t monitor_idx, const D3D11_TEXTURE2D_DESC& gpu_desc); // we set up staging texture if havent already
        void copy_bgra_to_master_rgb(const uint8_t* src_pointer, UINT row_pitch, size_t monitor_idx);    // DXGI captures in BGRA but models need it in RGB hence this function
        bool process_single_monitor(size_t monitor_idx);                                                 // a helper to be called in a loop inside capture_frame function

    public:
        Canvas() = default;
        ~Canvas() = default; // ComPtr handles RAII cleanup automatically

        // Disable copy constructor and assignment operator (as no copies should be made of this class objects)
        Canvas(const Canvas&) = delete;
        Canvas& operator=(const Canvas&) = delete;

        void cleanup();                                                                                  // destructor
        bool init();                                                                                     // Sets up factory, adapters, output, duplication devices, buffers and everything before capturing pipeline 
        bool capture_frame();                                                                            // main function that captures data, converts it and fills up the buffer 

        // --- Getters for nanobind / Python Interface ---
        std::pair<long, long> get_master_resolution() const { return master_resolution; }
        
        #ifdef CANVAS_RAM_RESIDENCE
            const uint8_t* get_master_buffer_ptr() const { return master_buffer.data(); }
            size_t get_master_buffer_size() const { return master_buffer.size(); }                       
            const std::vector<MonitorOffset>& get_monitor_offsets() const { return monitor_offsets; }

            // Helper to save current Master Buffer to disk for testing
            void saveMasterPpm(const char* filename);                                                        // helper for testing nothing more (will be removed in the future)
        #else   
            ComPtr<ID3D11Texture2D> get_staging_texture(size_t monitor_idx) const;                          // helper to get the staging texture for a specific monitor index
            ComPtr<ID3D11Texture2D> get_staging_texture(GpuVendor vendor) const;                            // helper to get the staging texture for a specific GPU vendor (NVIDIA, AMD, INTEL)
        #endif
    };
}