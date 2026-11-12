#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
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

    // One batch of decoded thumbnails handed from the worker thread to the UI thread.
    struct Chunk {
        std::vector<std::string> names;             // UTF-8 file names (successfully decoded files only)
        std::vector<std::vector<uint8_t>> pixels;   // parallel to names
        size_t failed = 0;                          // files of this batch that could not be decoded
    };

    // State of one folder load. Worker side: everything atomic / under `mutex`. UI side: the plain fields.
    struct LoadJob {
        std::atomic<bool> enumerated{false};  // `total` is valid
        std::atomic<size_t> total{0};
        std::atomic<size_t> decoded{0};       // files finished (ok or failed) by the worker
        std::atomic<bool> workerDone{false};
        std::atomic<bool> truncated{false};   // folder had more images than free GPU descriptor slots
        std::mutex mutex;
        std::deque<Chunk> ready;              // guarded by `mutex`

        size_t loaded = 0;                    // UI thread only
        size_t failed = 0;                    // UI thread only
        bool cancelled = false;               // UI thread only
        std::jthread thread;                  // keep last: destroyed (stopped + joined) first
    };

    void OpenFolder();
    void StartLoad(const std::filesystem::path& dir);
    void PumpLoading();                       // UI thread: uploads finished batches, finishes the job
    void CancelLoad();
    void FinishLoad(const char* error = nullptr);
    static void LoadWorker(LoadJob& job, const std::filesystem::path& dir, size_t capacity, std::stop_token st);
    void DrawLoadProgress();
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

    std::unique_ptr<LoadJob> job_;  // non-null while a folder is loading

    std::string status_;
    bool reqOpen_ = false;
    bool reqSave_ = false;
};
