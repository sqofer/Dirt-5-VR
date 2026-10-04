#pragma once
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>
inline void logGpuBudget(ID3D12CommandQueue* queue,std::ofstream& log,const char* phase){
 Microsoft::WRL::ComPtr<ID3D12Device> device;Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
 if(!queue||FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))||FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))){log<<"GPU_BUDGET_UNAVAILABLE phase="<<phase<<std::endl;return;}
 for(auto segment:{DXGI_MEMORY_SEGMENT_GROUP_LOCAL,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}){
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};auto hr=adapter->QueryVideoMemoryInfo(0,segment,&info);
  if(SUCCEEDED(hr))log<<"GPU_BUDGET phase="<<phase<<" segment="<<static_cast<UINT>(segment)<<" budgetBytes="<<info.Budget<<" processUsageBytes="<<info.CurrentUsage<<" reservationAvailableBytes="<<info.AvailableForReservation<<std::endl;
 }
}
