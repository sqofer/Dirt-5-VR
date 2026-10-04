#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <vector>

// Preserve a completed eye before the engine reuses its output surface.
// The source MUST be on this direct queue, in PRESENT/COMMON, with all
// producer-queue waits already submitted. This does not render an eye,
// resize the game, or establish cross-queue synchronization by itself.
class EyeGpuStore {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device_;Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12CommandAllocator> allocator_;Ptr<ID3D12GraphicsCommandList> list_;
    Ptr<ID3D12Fence> fence_;Ptr<ID3D12Resource> pendingSource_;
    std::array<Ptr<ID3D12Resource>,2> eye_,readback_;
    std::array<bool,2> valid_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT width_=0,height_=0;DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    UINT64 serial_=0,bytes_=0;HANDLE event_=nullptr;bool submitted_=false,ready_=false,readbackEnabled_=true;
    static void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&b);
    }
public:
    EyeGpuStore()=default;
    EyeGpuStore(const EyeGpuStore&)=delete;EyeGpuStore& operator=(const EyeGpuStore&)=delete;
    ~EyeGpuStore(){
        // A timed-out submission must never lose its GPU resources/allocator.
        // This object is owned for process lifetime by the injected probe.
        if(submitted_&&fence_&&fence_->GetCompletedValue()<serial_){
            pendingSource_.Detach();allocator_.Detach();list_.Detach();fence_.Detach();
            for(auto& e:eye_)e.Detach();for(auto& r:readback_)r.Detach();
            queue_.Detach();device_.Detach();event_=nullptr;
        }
        if(event_)CloseHandle(event_);
    }
    HRESULT setup(ID3D12Device* device,ID3D12CommandQueue* queue,UINT width,UINT height,DXGI_FORMAT format,bool enableReadback=true){
        if(device_||!device||!queue||!width||!height||width>8192||height>8192||
           format!=DXGI_FORMAT_R8G8B8A8_UNORM||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
        Ptr<ID3D12Device> queueDevice;auto hr=queue->GetDevice(IID_PPV_ARGS(&queueDevice));if(FAILED(hr))return hr;
        Ptr<IUnknown> a,b;device->QueryInterface(IID_PPV_ARGS(&a));queueDevice.As(&b);if(a.Get()!=b.Get())return E_INVALIDARG;
        device_=device;queue_=queue;width_=width;height_=height;format_=format;readbackEnabled_=enableReadback;
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_));if(FAILED(hr))return hr;
        hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&list_));if(FAILED(hr))return hr;
        hr=list_->Close();if(FAILED(hr))return hr;
        hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_));if(FAILED(hr))return hr;
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event_)return HRESULT_FROM_WIN32(GetLastError());
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=width;texture.Height=height;texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=format;texture.SampleDesc.Count=1;
        device->GetCopyableFootprints(&texture,0,1,0,&footprint_,nullptr,nullptr,&bytes_);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(auto& e:eye_){hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&e));if(FAILED(hr))return hr;}
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes_;buffer.Height=1;
        buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hp.Type=D3D12_HEAP_TYPE_READBACK;
        if(readbackEnabled_)for(auto& r:readback_){hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r));if(FAILED(hr))return hr;}
        ready_=true;return S_OK;
    }
    HRESULT finish(DWORD timeoutMs){
        if(!submitted_)return S_OK;
        auto value=fence_->GetCompletedValue();if(value==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(value<serial_){auto hr=fence_->SetEventOnCompletion(serial_,event_);if(FAILED(hr))return hr;
            if(WaitForSingleObject(event_,timeoutMs)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            if(fence_->GetCompletedValue()==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        }
        submitted_=false;pendingSource_.Reset();return S_OK;
    }
    HRESULT capture(unsigned eye,ID3D12Resource* source,DWORD timeoutMs=2000){
        if(eye>1||!source||!ready_||submitted_)return E_INVALIDARG;
        auto desc=source->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=width_||desc.Height!=height_||
            desc.Format!=format_||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1)return E_INVALIDARG;
        Ptr<ID3D12Device> sourceDevice;auto hr=source->GetDevice(IID_PPV_ARGS(&sourceDevice));if(FAILED(hr))return hr;
        Ptr<IUnknown> a,b;device_.As(&a);sourceDevice.As(&b);if(a.Get()!=b.Get())return E_INVALIDARG;
        valid_[eye]=false;hr=allocator_->Reset();if(FAILED(hr))return hr;hr=list_->Reset(allocator_.Get(),nullptr);if(FAILED(hr))return hr;
        transition(list_.Get(),source,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list_.Get(),eye_[eye].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyResource(eye_[eye].Get(),source);
        transition(list_.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        if(readbackEnabled_){
        transition(list_.Get(),eye_[eye].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback_[eye].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint_;
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=eye_[eye].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(list_.Get(),eye_[eye].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        }else transition(list_.Get(),eye_[eye].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        hr=list_->Close();if(FAILED(hr))return hr;
        pendingSource_=source;submitted_=true;++serial_;
        ID3D12CommandList* lists[]={list_.Get()};queue_->ExecuteCommandLists(1,lists);
        hr=queue_->Signal(fence_.Get(),serial_);if(FAILED(hr))return hr;
        hr=finish(timeoutMs);if(SUCCEEDED(hr))valid_[eye]=true;return hr;
    }
    HRESULT pixels(unsigned eye,std::vector<BYTE>& rgba){
        if(eye>1||!valid_[eye]||submitted_||!readbackEnabled_)return E_INVALIDARG;
        void* memory=nullptr;D3D12_RANGE range{static_cast<SIZE_T>(footprint_.Offset),static_cast<SIZE_T>(bytes_)};
        auto hr=readback_[eye]->Map(0,&range,&memory);if(FAILED(hr))return hr;
        try{rgba.resize(static_cast<size_t>(width_)*height_*4);}catch(...){D3D12_RANGE empty{0,0};readback_[eye]->Unmap(0,&empty);return E_OUTOFMEMORY;}
        auto source=static_cast<const BYTE*>(memory)+footprint_.Offset;
        for(UINT y=0;y<height_;++y)memcpy(rgba.data()+static_cast<size_t>(y)*width_*4,source+static_cast<size_t>(y)*footprint_.Footprint.RowPitch,static_cast<size_t>(width_)*4);
        D3D12_RANGE empty{0,0};readback_[eye]->Unmap(0,&empty);return S_OK;
    }
    ID3D12Resource* texture(unsigned eye)const{return eye<2&&valid_[eye]?eye_[eye].Get():nullptr;}
};
