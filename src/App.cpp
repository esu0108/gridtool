#include "App.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <execution>
#include <filesystem>
#include <numeric>

#include "BmpWriter.h"
#include "Dialogs.h"
#include "ImageLoader.h"
#include "imgui.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kPayloadPool = "POOL_IMG";  // from the left list: int pool index
constexpr const char* kPayloadCell = "CELL_IDX";  // from a grid cell:   int cell index

std::string ToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

ImTextureID TexId(const GpuTexture& t) { return static_cast<ImTextureID>(t.srv.ptr); }

}  // namespace

App::App(HWND hwnd, D3D12Context& d3d, float uiScale)
    : hwnd_(hwnd),
      d3d_(d3d),
      cell_(static_cast<float>(kThumbSize) * uiScale),
      scale_(uiScale),
      leftWidth_((static_cast<float>(kThumbSize) * uiScale + 8.0f * uiScale) * 4.0f + 40.0f * uiScale),
      cells_(static_cast<size_t>(kMaxGrid) * kMaxGrid, -1) {}

// ---------------------------------------------------------------------------------------------
// Deferred actions (dialogs / IO). Run outside of an ImGui frame.
// ---------------------------------------------------------------------------------------------

void App::RunPending() {
    if (reqOpen_) {
        reqOpen_ = false;
        if (!job_) OpenFolder();
    }
    if (reqSave_) {
        reqSave_ = false;
        ExportBmp();
    }
    PumpLoading();
}

// ---------------------------------------------------------------------------------------------
// Folder loading: enumerate + decode on a worker thread, upload to the GPU on the UI thread.
//
//   worker:  EnumerateImages -> decode/resize 64 files at a time (parallel) -> push Chunk to `ready`
//   UI:      each frame, take up to kUploadPerFrame finished thumbnails -> CreateTextures -> show them
//
// Thumbnails therefore appear while the rest is still being decoded, and the UI never blocks.
// ---------------------------------------------------------------------------------------------

void App::OpenFolder() {
    const auto dir = PickFolder(hwnd_);
    if (!dir) return;
    StartLoad(*dir);
}

void App::StartLoad(const fs::path& dir) {
    folderLabel_ = ToUtf8(dir);
    list_.clear();
    status_.clear();

    auto job = std::make_unique<LoadJob>();
    LoadJob* j = job.get();
    const size_t capacity = d3d_.FreeSrvSlots();
    job->thread = std::jthread([j, dir, capacity](std::stop_token st) { LoadWorker(*j, dir, capacity, st); });
    job_ = std::move(job);
}

void App::LoadWorker(LoadJob& job, const fs::path& dir, size_t capacity, std::stop_token st) {
    constexpr size_t kChunk = 64;

    std::vector<fs::path> files;
    try {
        files = EnumerateImages(dir);
    } catch (...) {
        files.clear();
    }
    if (files.size() > capacity) {
        files.resize(capacity);
        job.truncated = true;
    }
    job.total.store(files.size());
    job.enumerated.store(true);

    for (size_t base = 0; base < files.size() && !st.stop_requested(); base += kChunk) {
        const size_t n = std::min(kChunk, files.size() - base);
        std::vector<std::vector<uint8_t>> px(n);
        std::vector<size_t> idx(n);
        std::iota(idx.begin(), idx.end(), size_t{0});

        std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t i) {
            if (!st.stop_requested()) {
                try {
                    if (!LoadThumbnail(files[base + i], px[i])) px[i].clear();
                } catch (...) {  // e.g. bad_alloc on a huge image; an exception escaping a parallel algorithm would terminate
                    px[i].clear();
                }
            }
            job.decoded.fetch_add(1, std::memory_order_relaxed);
        });
        if (st.stop_requested()) break;

        Chunk ck;
        for (size_t i = 0; i < n; ++i) {
            if (px[i].empty()) {
                ++ck.failed;
            } else {
                ck.names.push_back(ToUtf8(files[base + i].filename()));
                ck.pixels.push_back(std::move(px[i]));
            }
        }
        std::lock_guard<std::mutex> lk(job.mutex);
        job.ready.push_back(std::move(ck));
    }
    job.workerDone.store(true, std::memory_order_release);
}

void App::PumpLoading() {
    if (!job_) return;
    constexpr size_t kUploadPerFrame = 256;

    // Read before taking the queue: if the worker was already done, every chunk it produced is in `ready`.
    const bool workerDone = job_->workerDone.load(std::memory_order_acquire);

    std::vector<Chunk> batch;
    size_t count = 0;
    bool drained = false;
    {
        std::lock_guard<std::mutex> lk(job_->mutex);
        if (job_->cancelled) job_->ready.clear();  // cancelled: discard whatever is still queued
        while (!job_->ready.empty() && count < kUploadPerFrame) {
            count += job_->ready.front().pixels.size();
            batch.push_back(std::move(job_->ready.front()));
            job_->ready.pop_front();
        }
        drained = job_->ready.empty();
    }

    if (!batch.empty()) {
        std::vector<const uint8_t*> ptrs;
        for (const Chunk& ck : batch) {
            job_->failed += ck.failed;
            for (const auto& px : ck.pixels) ptrs.push_back(px.data());
        }
        if (!ptrs.empty()) {
            std::vector<GpuTexture> textures;
            if (!d3d_.CreateTextures(ptrs, textures)) {
                FinishLoad("テクスチャの作成に失敗しました。");
                return;
            }
            size_t k = 0;
            for (Chunk& ck : batch) {
                for (size_t i = 0; i < ck.pixels.size(); ++i, ++k) {
                    Image img;
                    img.name = std::move(ck.names[i]);
                    img.rgba = std::move(ck.pixels[i]);
                    img.tex = std::move(textures[k]);
                    list_.push_back(static_cast<int>(pool_.size()));
                    pool_.push_back(std::move(img));
                }
            }
            job_->loaded += ptrs.size();
        }
    }

    if (workerDone && drained) FinishLoad();
}

void App::CancelLoad() {
    if (!job_) return;
    job_->cancelled = true;
    job_->thread.request_stop();
}

void App::FinishLoad(const char* error) {
    if (!job_) return;
    job_->thread.request_stop();  // no-op when already finished; makes the join below quick after an error

    const size_t loaded = job_->loaded;
    const size_t failed = job_->failed;
    const bool truncated = job_->truncated.load();
    const bool cancelled = job_->cancelled;
    const bool noFiles = job_->enumerated.load() && job_->total.load() == 0;
    job_.reset();  // joins the worker

    if (loaded > 0) {
        const TextureMemoryStats m = d3d_.GetTextureMemoryStats();
        char buf[160];
        std::snprintf(buf, sizeof(buf), "テクスチャVRAM: %.2f MB 確保 / %.2f MB 使用 (%u 枚, ヒープ %u)",
                      static_cast<double>(m.blockBytes) / (1024.0 * 1024.0),
                      static_cast<double>(m.allocationBytes) / (1024.0 * 1024.0), m.allocationCount, m.blockCount);
        memInfo_ = buf;
    }

    if (error) {
        status_ = error;
    } else if (noFiles) {
        status_ = "このフォルダに対応する画像(png/jpg/bmp/tga/gif)がありません。";
    } else if (cancelled) {
        status_ = "読み込みをキャンセルしました。(" + std::to_string(loaded) + " 枚読み込み済み)";
    } else {
        status_ = std::to_string(loaded) + " 枚読み込みました。";
        if (failed > 0) status_ += " (読み込み失敗: " + std::to_string(failed) + " 枚)";
        if (truncated) status_ += " (上限に達したため一部を読み込んでいません)";
    }
}

void App::DrawLoadProgress() {
    const bool known = job_->enumerated.load();
    if (!known) {
        // Indeterminate bar while the folder is being scanned.
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, 0), "フォルダを走査中...");
    } else {
        const size_t total = job_->total.load();
        const size_t done = std::min(job_->decoded.load(), total);
        char text[64];
        std::snprintf(text, sizeof(text), "%zu / %zu", done, total);
        ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 1.0f,
                           ImVec2(-FLT_MIN, 0), text);
    }
    if (job_->cancelled) {
        ImGui::TextDisabled("キャンセル中...");
    } else if (ImGui::Button("キャンセル")) {
        CancelLoad();
    }
}

void App::ExportBmp() {
    const auto path = PickSaveBmp(hwnd_);
    if (!path) return;

    const int width = cols_ * kThumbSize;
    const int height = rows_ * kThumbSize;
    std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3, 0);  // empty cells stay black

    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            const int img = cells_[static_cast<size_t>(r) * kMaxGrid + c];
            if (img < 0) continue;
            const std::vector<uint8_t>& src = pool_[img].rgba;
            for (int y = 0; y < kThumbSize; ++y) {
                for (int x = 0; x < kThumbSize; ++x) {
                    const uint8_t* s = src.data() + (static_cast<size_t>(y) * kThumbSize + x) * 4;
                    uint8_t* d = bgr.data() +
                                 (static_cast<size_t>(r * kThumbSize + y) * width + (c * kThumbSize + x)) * 3;
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                }
            }
        }
    }

    if (WriteBmp24(*path, width, height, bgr.data())) {
        status_ = "保存しました: " + ToUtf8(*path) + " (" + std::to_string(width) + " x " + std::to_string(height) + ")";
    } else {
        status_ = "BMP の保存に失敗しました: " + ToUtf8(*path);
    }
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------

void App::DrawUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##root", nullptr, flags);

    DrawLeftPane();
    ImGui::SameLine();
    DrawRightPane();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void App::DrawLeftPane() {
    ImGui::BeginChild("left", ImVec2(leftWidth_, 0), ImGuiChildFlags_Borders);

    ImGui::BeginDisabled(job_ != nullptr);
    if (ImGui::Button("フォルダを開く")) reqOpen_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d 枚", static_cast<int>(list_.size()));
    if (!folderLabel_.empty()) ImGui::TextWrapped("%s", folderLabel_.c_str());
    if (!memInfo_.empty()) ImGui::TextDisabled("%s", memInfo_.c_str());
    if (job_) DrawLoadProgress();
    ImGui::Separator();

    ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_None);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, static_cast<int>((avail + spacing) / (cell_ + spacing)));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    for (size_t i = 0; i < list_.size(); ++i) {
        const int poolIdx = list_[i];
        const Image& img = pool_[poolIdx];

        if (i % perRow != 0) ImGui::SameLine();
        ImGui::PushID(poolIdx);
        ImGui::ImageButton("##thumb", TexId(img.tex), ImVec2(cell_, cell_));

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(kPayloadPool, &poolIdx, sizeof(int));
            ImGui::Image(TexId(img.tex), ImVec2(cell_, cell_));
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload()) ImGui::SetTooltip("%s", img.name.c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    ImGui::EndChild();
    ImGui::EndChild();
}

void App::DrawRightPane() {
    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::SetNextItemWidth(120.0f * scale_);
    if (ImGui::InputInt("列", &cols_)) cols_ = std::clamp(cols_, 1, kMaxGrid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f * scale_);
    if (ImGui::InputInt("行", &rows_)) rows_ = std::clamp(rows_, 1, kMaxGrid);
    cols_ = std::clamp(cols_, 1, kMaxGrid);
    rows_ = std::clamp(rows_, 1, kMaxGrid);

    ImGui::SameLine();
    if (ImGui::Button("出力")) reqSave_ = true;
    ImGui::SameLine();
    if (ImGui::Button("全クリア")) std::fill(cells_.begin(), cells_.end(), -1);

    ImGui::TextDisabled("出力サイズ: %d x %d px    右クリック: マスを空にする", cols_ * kThumbSize, rows_ * kThumbSize);
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::Separator();

    ImGui::BeginChild("grid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    DrawGrid();
    ImGui::EndChild();

    ImGui::EndChild();
}

void App::DrawGrid() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 cellSize(cell_, cell_);

    // Drop-target highlight is drawn by us after all cells: ImGui's default rectangle is drawn at the moment the
    // cell is processed, so the cells after it (right / below) paint over its right and bottom edges.
    bool hasTarget = false;
    ImVec2 target0, target1;

    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            const int idx = r * kMaxGrid + c;
            const ImVec2 p0(origin.x + c * cell_, origin.y + r * cell_);
            const ImVec2 p1(p0.x + cell_, p0.y + cell_);

            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(idx);
            ImGui::InvisibleButton("cell", cellSize);

            // Right click: clear the cell.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) cells_[idx] = -1;

            const int img = cells_[idx];
            dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
            if (img >= 0) dl->AddImage(TexId(pool_[img].tex), p0, p1);
            dl->AddRect(p0, p1, IM_COL32(80, 80, 80, 255));

            // Drag a placed image to another cell.
            if (img >= 0 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload(kPayloadCell, &idx, sizeof(int));
                ImGui::Image(TexId(pool_[img].tex), cellSize);
                ImGui::EndDragDropSource();
            }

            // Drop target: from the list (place / replace) or from another cell (move / replace).
            if (ImGui::BeginDragDropTarget()) {
                constexpr ImGuiDragDropFlags kAccept =
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                const ImGuiPayload* fromPool = ImGui::AcceptDragDropPayload(kPayloadPool, kAccept);
                const ImGuiPayload* fromCell = fromPool ? nullptr : ImGui::AcceptDragDropPayload(kPayloadCell, kAccept);
                if (fromPool || fromCell) {
                    hasTarget = true;
                    target0 = p0;
                    target1 = p1;
                }
                if (fromPool && fromPool->IsDelivery()) {
                    cells_[idx] = *static_cast<const int*>(fromPool->Data);
                } else if (fromCell && fromCell->IsDelivery()) {
                    const int src = *static_cast<const int*>(fromCell->Data);
                    if (src != idx) {
                        cells_[idx] = cells_[src];
                        cells_[src] = -1;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (img >= 0 && ImGui::IsItemHovered() && !ImGui::GetDragDropPayload()) {
                ImGui::SetTooltip("%s", pool_[img].name.c_str());
            }
            ImGui::PopID();
        }
    }

    // Highlight the hovered drop target on all four sides (inset so it is never clipped at the grid edge).
    if (hasTarget) {
        dl->AddRect(ImVec2(target0.x + 1.0f, target0.y + 1.0f), ImVec2(target1.x - 1.0f, target1.y - 1.0f),
                    ImGui::GetColorU32(ImGuiCol_DragDropTarget), 0.0f, 0, 2.0f);
    }

    // Give the scroll area its full extent.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(cols_ * cell_, rows_ * cell_));
}
