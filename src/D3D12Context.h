#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

struct GpuTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
};

// Minimal D3D12 device / swap chain wrapper plus helper for uploading 64x64 RGBA textures.
class D3D12Context {
public:
    static constexpr UINT kFrameCount = 3;
    static constexpr UINT kSrvCapacity = 4096;
    static constexpr UINT kImGuiSrvReserve = 16;  // descriptors kept free for ImGui's own (font atlas) textures
    static constexpr UINT kTexSize = 64;

    bool Init(HWND hwnd, UINT width, UINT height);
    void Shutdown();
    void Resize(UINT width, UINT height);
    void WaitForGpu();

    // Returns an open command list with the back buffer bound and cleared.
    ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4]);
    void EndFrame();

    // Uploads N textures of kTexSize x kTexSize RGBA8 (tightly packed). Blocks until the GPU is done.
    bool CreateTextures(const std::vector<const uint8_t*>& rgba, std::vector<GpuTexture>& out);

    ID3D12Device* Device() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    ID3D12DescriptorHeap* SrvHeap() const { return srvHeap_.Get(); }
    DXGI_FORMAT BackBufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    // Number of descriptors still available for user textures (ImGui's reserve excluded).
    UINT FreeSrvSlots() const;

    // Shader-visible SRV descriptor allocator, shared by our textures and the ImGui DX12 backend.
    bool AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

private:
    void CreateRenderTargets();
    void ReleaseRenderTargets();
    void WaitFence(UINT64 value);

    struct Frame {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 fenceValue = 0;
    };

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> backBuffers_;
    std::array<Frame, kFrameCount> frames_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    UINT frameIndex_ = 0;
    UINT rtvInc_ = 0;
    UINT srvInc_ = 0;
    UINT nextSrv_ = 0;
    std::vector<UINT> freeSrv_;
};
