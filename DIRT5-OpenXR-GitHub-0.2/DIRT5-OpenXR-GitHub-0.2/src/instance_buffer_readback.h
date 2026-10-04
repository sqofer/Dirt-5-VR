#pragma once
#include "eye_gpu_store.h"

// Bounded diagnostic copy. The caller supplies the observed resource state and
// uses the same direct queue after native rendering has submitted its waits.
class InstanceBufferReadback {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12Resource> source_,readback_;
    Ptr<ID3D12CommandAllocator> allocator_;
    Ptr<ID3D12GraphicsCommandList> list_;
    Ptr<ID3D12Fence> fence_;
    HANDLE event_=nullptr;UINT64 bytes_=0,serial_=0;bool submitted_=false;
public:
    InstanceBufferReadback()=default;
    InstanceBufferReadback(const InstanceBufferReadback&)=delete;
    InstanceBufferReadback& operator=(const InstanceBufferReadback&)=delete;
    ~InstanceBufferReadback(){
        // Preserve references if an exceptional timeout leaves work on the GPU.
        if(submitted_&&fence_&&fence_->GetCompletedValue()<serial_){
            queue_.Detach();source_.Detach();readback_.Detach();
            allocator_.Detach();list_.Detach();fence_.Detach();event_=nullptr;
        }
        if(event_)CloseHandle(event_);
    }
    HRESULT setup(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* source,UINT64 expectedBytes=UINT64(98304)*136){
        if(!device||!queue||!source)return E_INVALIDARG;
        auto desc=source->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||!expectedBytes||expectedBytes>UINT64(98304)*136||desc.Width!=expectedBytes)return E_INVALIDARG;
        Ptr<ID3D12Device> other;auto hr=source->GetDevice(IID_PPV_ARGS(&other));if(FAILED(hr))return hr;
        Ptr<IUnknown> a,b;device->QueryInterface(IID_PPV_ARGS(&a));other.As(&b);if(a.Get()!=b.Get())return E_INVALIDARG;
        source_=source;queue_=queue;bytes_=desc.Width;
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_));if(FAILED(hr))return hr;
        hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&list_));if(FAILED(hr))return hr;
        hr=list_->Close();if(FAILED(hr))return hr;
        hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_));if(FAILED(hr))return hr;
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event_)return HRESULT_FROM_WIN32(GetLastError());
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        return device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_));
    }
    HRESULT capture(D3D12_RESOURCE_STATES state,std::vector<BYTE>& bytes){
        if(!source_||!readback_||(state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE&&state!=D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER))return E_UNEXPECTED;
        auto hr=allocator_->Reset();if(FAILED(hr))return hr;hr=list_->Reset(allocator_.Get(),nullptr);if(FAILED(hr))return hr;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={source_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};list_->ResourceBarrier(1,&barrier);
        list_->CopyBufferRegion(readback_.Get(),0,source_.Get(),0,bytes_);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=state;list_->ResourceBarrier(1,&barrier);
        hr=list_->Close();if(FAILED(hr))return hr;
        submitted_=true;++serial_;
        ID3D12CommandList* lists[]={list_.Get()};queue_->ExecuteCommandLists(1,lists);
        hr=queue_->Signal(fence_.Get(),serial_);if(FAILED(hr))return hr;
        if(fence_->GetCompletedValue()<serial_){hr=fence_->SetEventOnCompletion(serial_,event_);if(FAILED(hr))return hr;if(WaitForSingleObject(event_,2000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        if(fence_->GetCompletedValue()==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        submitted_=false;
        bytes.resize(static_cast<size_t>(bytes_));void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes_)};
        hr=readback_->Map(0,&range,&mapped);if(FAILED(hr))return hr;
        memcpy(bytes.data(),mapped,bytes.size());D3D12_RANGE empty{0,0};readback_->Unmap(0,&empty);return S_OK;
    }
};
