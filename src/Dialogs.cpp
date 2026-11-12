#include "Dialogs.h"

#include <ShObjIdl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

std::optional<std::filesystem::path> GetResultPath(IFileDialog* dlg) {
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return std::nullopt;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return std::nullopt;
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

}  // namespace

std::optional<std::filesystem::path> PickFolder(HWND owner) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return std::nullopt;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"画像フォルダを選択");
    if (FAILED(dlg->Show(owner))) return std::nullopt;  // cancelled
    return GetResultPath(dlg.Get());
}

std::optional<std::filesystem::path> PickSaveBmp(HWND owner) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return std::nullopt;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

    const COMDLG_FILTERSPEC spec[] = {{L"BMP 画像 (*.bmp)", L"*.bmp"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetFileTypeIndex(1);
    dlg->SetDefaultExtension(L"bmp");
    dlg->SetFileName(L"output.bmp");
    dlg->SetTitle(L"BMP として保存");
    if (FAILED(dlg->Show(owner))) return std::nullopt;  // cancelled
    return GetResultPath(dlg.Get());
}
