#include <Windows.h>
#include <objbase.h>

#include <filesystem>

#include "App.h"
#include "D3D12Context.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

UINT g_resizeW = 0;
UINT g_resizeH = 0;
bool g_minimized = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;

    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            g_minimized = true;
        } else {
            g_minimized = false;
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // disable ALT application menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// A Japanese-capable system font is required for the UI labels.
// (Since ImGui 1.92 glyphs are rasterized on demand, so no glyph ranges are needed.
//  The DPI scale is applied through style.FontScaleDpi.)
void LoadJapaneseFont(float pixelSize) {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            if (io.Fonts->AddFontFromFileTTF(path, pixelSize)) return;
        }
    }
    io.Fonts->AddFontDefault();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    ImGui_ImplWin32_EnableDpiAwareness();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ImageGridToolWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Image Grid Tool", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1400, 900, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    RECT rc{};
    GetClientRect(hwnd, &rc);

    int exitCode = 0;
    {
        D3D12Context d3d;
        if (!d3d.Init(hwnd, static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top))) {
            MessageBoxW(hwnd, L"Direct3D 12 の初期化に失敗しました。", L"Image Grid Tool", MB_ICONERROR);
            return 1;
        }

        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);

        const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleDpi = scale;
        LoadJapaneseFont(18.0f);

        ImGui_ImplWin32_Init(hwnd);

        ImGui_ImplDX12_InitInfo initInfo;
        initInfo.Device = d3d.Device();
        initInfo.CommandQueue = d3d.Queue();
        initInfo.NumFramesInFlight = static_cast<int>(D3D12Context::kFrameCount);
        initInfo.RTVFormat = d3d.BackBufferFormat();
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.UserData = &d3d;
        initInfo.SrvDescriptorHeap = d3d.SrvHeap();
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            static_cast<D3D12Context*>(info->UserData)->AllocSrv(cpu, gpu);
        };
        initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                          D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
            static_cast<D3D12Context*>(info->UserData)->FreeSrv(cpu, gpu);
        };
        ImGui_ImplDX12_Init(&initInfo);

        {
            App app(hwnd, d3d, scale);

            const float clear[4] = {0.10f, 0.10f, 0.12f, 1.0f};
            MSG msg{};
            while (msg.message != WM_QUIT) {
                if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    continue;
                }
                if (g_minimized) {
                    Sleep(10);
                    continue;
                }
                if (g_resizeW != 0 && g_resizeH != 0) {
                    d3d.Resize(g_resizeW, g_resizeH);
                    g_resizeW = g_resizeH = 0;
                }

                // Dialogs / file IO / texture uploads happen here, outside of an ImGui frame.
                app.RunPending();

                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                app.DrawUI();
                ImGui::Render();

                ID3D12GraphicsCommandList* cmd = d3d.BeginFrame(clear);
                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
                d3d.EndFrame();
            }
            exitCode = static_cast<int>(msg.wParam);

            d3d.WaitForGpu();
        }  // App (and its GPU textures) destroyed here, before the device

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        d3d.Shutdown();
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    CoUninitialize();
    return exitCode;
}
