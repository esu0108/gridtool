#include "App.h"

#include <algorithm>
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
        OpenFolder();
    }
    if (reqSave_) {
        reqSave_ = false;
        ExportBmp();
    }
}

void App::OpenFolder() {
    const auto dir = PickFolder(hwnd_);
    if (!dir) return;

    std::vector<fs::path> files = EnumerateImages(*dir);
    folderLabel_ = ToUtf8(*dir);
    list_.clear();

    if (files.empty()) {
        status_ = "このフォルダに対応する画像(png/jpg/bmp/tga/gif)がありません。";
        return;
    }

    bool truncated = false;
    const size_t capacity = d3d_.FreeSrvSlots();
    if (files.size() > capacity) {
        files.resize(capacity);
        truncated = true;
    }

    // Decode + resize in parallel.
    std::vector<std::vector<uint8_t>> pixels(files.size());
    std::vector<size_t> order(files.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t i) {
        if (!LoadThumbnail(files[i], pixels[i])) pixels[i].clear();
    });

    std::vector<size_t> okIndex;
    std::vector<const uint8_t*> ptrs;
    for (size_t i = 0; i < files.size(); ++i) {
        if (pixels[i].empty()) continue;
        okIndex.push_back(i);
        ptrs.push_back(pixels[i].data());
    }
    const size_t failed = files.size() - okIndex.size();

    std::vector<GpuTexture> textures;
    if (!d3d_.CreateTextures(ptrs, textures)) {
        status_ = "テクスチャの作成に失敗しました。";
        return;
    }

    for (size_t k = 0; k < okIndex.size(); ++k) {
        Image img;
        img.name = ToUtf8(files[okIndex[k]].filename());
        img.rgba = std::move(pixels[okIndex[k]]);
        img.tex = std::move(textures[k]);
        list_.push_back(static_cast<int>(pool_.size()));
        pool_.push_back(std::move(img));
    }

    status_ = std::to_string(okIndex.size()) + " 枚読み込みました。";
    if (failed > 0) status_ += " (読み込み失敗: " + std::to_string(failed) + " 枚)";
    if (truncated) status_ += " (上限に達したため一部を読み込んでいません)";
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

    if (ImGui::Button("フォルダを開く")) reqOpen_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%d 枚", static_cast<int>(list_.size()));
    if (!folderLabel_.empty()) ImGui::TextWrapped("%s", folderLabel_.c_str());
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
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kPayloadPool)) {
                    cells_[idx] = *static_cast<const int*>(pl->Data);
                } else if (const ImGuiPayload* pc = ImGui::AcceptDragDropPayload(kPayloadCell)) {
                    const int src = *static_cast<const int*>(pc->Data);
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

    // Give the scroll area its full extent.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(cols_ * cell_, rows_ * cell_));
}
