#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "D3D12Context.h"

constexpr int kMaxGrid = 64;  // max columns / rows

class App {
public:
    App(HWND hwnd, D3D12Context& d3d, float uiScale);

    // Runs modal dialogs / file IO / GPU uploads requested by the UI. Call once per frame BEFORE ImGui::NewFrame().
    void RunPending();

    // Builds the UI. Call between ImGui::NewFrame() and ImGui::Render().
    void DrawUI();

private:
    struct Image {
        std::string name;            // UTF-8 file name (tooltip)
        std::vector<uint8_t> rgba;   // kThumbSize^2 * 4, opaque
        GpuTexture tex;
    };

    void OpenFolder();
    void ExportBmp();
    void DrawLeftPane();
    void DrawRightPane();
    void DrawGrid();

    HWND hwnd_;
    D3D12Context& d3d_;
    float cell_;       // on-screen size of one thumbnail / grid cell (64 * dpi scale)
    float scale_;
    float leftWidth_;

    std::vector<Image> pool_;  // every image ever loaded (grid cells refer to indices in here)
    std::vector<int> list_;    // images of the currently opened folder (indices into pool_)
    std::string folderLabel_;
    std::string memInfo_;  // GPU texture memory summary (refreshed after each load)

    // Fixed kMaxGrid x kMaxGrid storage, so shrinking the visible grid does not lose placements.
    std::vector<int> cells_;
    int cols_ = 8;
    int rows_ = 8;

    std::string status_;
    bool reqOpen_ = false;
    bool reqSave_ = false;
};
