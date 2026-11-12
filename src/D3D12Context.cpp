#include "D3D12Context.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

}  // namespace

bool D3D12Context::Init(HWND hwnd, UINT width, UINT height) {
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = BackBufferFormat();
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrameCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) return false;
    if (FAILED(sc1.As(&swap_))) return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = kFrameCount;
    if (FAILED(device_->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvHeap_)))) return false;
    rtvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvCapacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) return false;
    srvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (auto& f : frames_) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) return false;
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&cmd_)))) return false;
    cmd_->Close();

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) return false;

    CreateRenderTargets();
    return true;
}

void D3D12Context::Shutdown() {
    if (queue_ && fence_ && fenceEvent_) WaitForGpu();
    ReleaseRenderTargets();
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    cmd_.Reset();
    for (auto& f : frames_) f.allocator.Reset();
    srvHeap_.Reset();
    rtvHeap_.Reset();
    swap_.Reset();
    fence_.Reset();
    queue_.Reset();
    device_.Reset();
}

void D3D12Context::CreateRenderTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, h);
        h.ptr += rtvInc_;
    }
}

void D3D12Context::ReleaseRenderTargets() {
    for (auto& b : backBuffers_) b.Reset();
}

void D3D12Context::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    WaitForGpu();
    ReleaseRenderTargets();
    swap_->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTargets();
    for (auto& f : frames_) f.fenceValue = 0;
}

void D3D12Context::WaitFence(UINT64 value) {
    if (value != 0 && fence_->GetCompletedValue() < value) {
        fence_->SetEventOnCompletion(value, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void D3D12Context::WaitForGpu() {
    const UINT64 v = ++fenceValue_;
    queue_->Signal(fence_.Get(), v);
    WaitFence(v);
}

ID3D12GraphicsCommandList* D3D12Context::BeginFrame(const float clearColor[4]) {
    frameIndex_ = swap_->GetCurrentBackBufferIndex();
    Frame& f = frames_[frameIndex_];
    WaitFence(f.fenceValue);

    f.allocator->Reset();
    cmd_->Reset(f.allocator.Get(), nullptr);

    const D3D12_RESOURCE_BARRIER b =
        Transition(backBuffers_[frameIndex_].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd_->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(frameIndex_) * rtvInc_;
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    cmd_->SetDescriptorHeaps(1, heaps);
    return cmd_.Get();
}

void D3D12Context::EndFrame() {
    const D3D12_RESOURCE_BARRIER b =
        Transition(backBuffers_[frameIndex_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd_->ResourceBarrier(1, &b);
    cmd_->Close();

    ID3D12CommandList* lists[] = {cmd_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    swap_->Present(1, 0);

    const UINT64 v = ++fenceValue_;
    queue_->Signal(fence_.Get(), v);
    frames_[frameIndex_].fenceValue = v;
}

bool D3D12Context::CreateTextures(const std::vector<const uint8_t*>& rgba, std::vector<GpuTexture>& out) {
    out.clear();
    const UINT n = static_cast<UINT>(rgba.size());
    if (n == 0) return true;
    if (n > FreeSrvSlots()) return false;

    constexpr UINT64 kBytes = UINT64(kTexSize) * kTexSize * 4;  // 16384: multiple of 512 (placement alignment)

    // One big upload buffer holding every texture.
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kBytes * n;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    if (FAILED(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) {
        return false;
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return false;
    for (UINT i = 0; i < n; ++i) std::memcpy(mapped + kBytes * i, rgba[i], kBytes);
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kTexSize;
    texDesc.Height = kTexSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    std::vector<GpuTexture> result(n);
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(n);

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> cpuHandles;  // for rollback on failure
    cpuHandles.reserve(n);
    auto rollback = [&]() {
        for (size_t k = 0; k < cpuHandles.size(); ++k) FreeSrv(cpuHandles[k], result[k].srv);
    };

    for (UINT i = 0; i < n; ++i) {
        GpuTexture& t = result[i];
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if (!AllocSrv(&cpu, &gpu)) {
            rollback();
            return false;
        }
        t.srv = gpu;
        cpuHandles.push_back(cpu);

        if (FAILED(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&t.resource)))) {
            rollback();
            return false;
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = t.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = kBytes * i;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = kTexSize;
        src.PlacedFootprint.Footprint.Height = kTexSize;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = kTexSize * 4;  // 256: satisfies D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barriers.push_back(Transition(t.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(t.resource.Get(), &srv, cpu);
    }

    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    WaitForGpu();

    out = std::move(result);
    return true;
}

UINT D3D12Context::FreeSrvSlots() const {
    const UINT used = nextSrv_ - static_cast<UINT>(freeSrv_.size());
    const UINT limit = kSrvCapacity - kImGuiSrvReserve;
    return used >= limit ? 0 : limit - used;
}

bool D3D12Context::AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    UINT index;
    if (!freeSrv_.empty()) {
        index = freeSrv_.back();
        freeSrv_.pop_back();
    } else if (nextSrv_ < kSrvCapacity) {
        index = nextSrv_++;
    } else {
        return false;
    }
    *cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    *gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += SIZE_T(index) * srvInc_;
    gpu->ptr += UINT64(index) * srvInc_;
    return true;
}

void D3D12Context::FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    const SIZE_T start = srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr;
    freeSrv_.push_back(static_cast<UINT>((cpu.ptr - start) / srvInc_));
}
