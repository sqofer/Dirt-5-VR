#pragma once
#include "eye_gpu_store.h"
#include <mutex>
#include <unordered_map>

// Repeat only the verified game fullscreen colour-composition draw into a
// separate GPU surface. World geometry is still rendered by the two native
// scene passes. Reuse the game's colour processing and normalized texture UVs.
class NativeFinalEye {
 using OmFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
 using VpFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*);
 using ScFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*);
 using DrawFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
 template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
 struct ListState{UINT64 list=0;UINT count=0,vpCount=0,scCount=0;BOOL range=FALSE;std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> rtvs{};D3D12_CPU_DESCRIPTOR_HANDLE dsv{};std::array<D3D12_VIEWPORT,8> vp{};std::array<D3D12_RECT,8> sc{};};
 std::array<ListState,64> lists_{};std::mutex mutex_;
 Ptr<ID3D12DescriptorHeap> heap_;std::array<Ptr<ID3D12Resource>,2> targets_;
 EyeGpuStore store_;UINT width_=0,height_=0,desktopWidth_=0,desktopHeight_=0,stride_=0;
 bool directOutput_=false;std::array<bool,2> valid_{};
 bool independentConstants_=false;
 std::array<Ptr<ID3D12Resource>,2> colourConstants_;
 std::array<BYTE*,2> mappedConstants_{};
 std::array<UINT64,2> constantGpu_{},nativeConstantGpu_{};
 std::array<UINT,2> constantRoot_{};std::array<bool,2> constantsValid_{};
 std::array<std::atomic<UINT>,2> draws_{};std::array<std::atomic<UINT64>,2> producer_{},executed_{},queue_{};
 ListState* state(UINT64 list){for(auto& s:lists_)if(s.list==list)return &s;for(auto& s:lists_)if(!s.list){s.list=list;return &s;}return nullptr;}
public:
 std::atomic<UINT64> totalDraws{0};
 HRESULT setup(ID3D12Device* device,ID3D12CommandQueue* queue,UINT width,UINT height,UINT desktopWidth,UINT desktopHeight,bool directOutput=false,bool independentConstants=false){
  width_=width;height_=height;desktopWidth_=desktopWidth;desktopHeight_=desktopHeight;
  directOutput_=directOutput;HRESULT hr=S_OK;
  if(!directOutput_){hr=store_.setup(device,queue,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,false);if(FAILED(hr))return hr;}
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
  hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_));if(FAILED(hr))return hr;stride_=device->GetDescriptorHandleIncrementSize(hd.Type);
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=width;td.Height=height;td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};clear.Format=td.Format;clear.Color[3]=1;
  for(UINT eye=0;eye<2;++eye){hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COMMON,&clear,IID_PPV_ARGS(&targets_[eye]));if(FAILED(hr))return hr;auto rtv=heap_->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=eye*stride_;device->CreateRenderTargetView(targets_[eye].Get(),nullptr,rtv);}
  independentConstants_=independentConstants;
  if(independentConstants_){D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC cb{};cb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;cb.Width=256;cb.Height=1;cb.DepthOrArraySize=cb.MipLevels=1;cb.SampleDesc.Count=1;cb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   for(UINT eye=0;eye<2;++eye){hr=device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&cb,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&colourConstants_[eye]));if(FAILED(hr))return hr;D3D12_RANGE range{0,0};hr=colourConstants_[eye]->Map(0,&range,reinterpret_cast<void**>(&mappedConstants_[eye]));if(FAILED(hr))return hr;constantGpu_[eye]=colourConstants_[eye]->GetGPUVirtualAddress();}
  }
  return S_OK;
 }
 void begin(){for(UINT eye=0;eye<2;++eye){valid_[eye]=false;constantsValid_[eye]=false;draws_[eye]=0;producer_[eye]=executed_[eye]=queue_[eye]=0;}}
 bool setColourConstants(UINT eye,UINT root,UINT64 nativeGpu,const BYTE* bytes,UINT size){
  if(!independentConstants_||eye>1||root>=64||!nativeGpu||(nativeGpu&255)||!bytes||size!=128||!mappedConstants_[eye]||draws_[eye].load())return false;
  memcpy(mappedConstants_[eye],bytes,size);
  // This is a separate fullscreen output, not the desktop upscale. Preserve
  // exposure, grading and gamma; give it its own viewport and omit the game's
  // desktop-dependent sharpening, which can exceed 1 after square resizing.
  memcpy(mappedConstants_[eye]+32,&width_,4);memcpy(mappedConstants_[eye]+36,&height_,4);
  float inverseWidth=1.f/width_,inverseHeight=1.f/height_,noSharpen=0.f;
  memcpy(mappedConstants_[eye]+40,&inverseWidth,4);memcpy(mappedConstants_[eye]+44,&inverseHeight,4);memcpy(mappedConstants_[eye]+64,&noSharpen,4);
  constantRoot_[eye]=root;nativeConstantGpu_[eye]=nativeGpu;constantsValid_[eye]=true;return true;
 }
 void om(ID3D12GraphicsCommandList* list,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,BOOL range,const D3D12_CPU_DESCRIPTOR_HANDLE* dsv){
  std::lock_guard<std::mutex> lock(mutex_);auto s=state(reinterpret_cast<UINT64>(list));if(!s)return;s->count=count;s->range=range;s->dsv=dsv?*dsv:D3D12_CPU_DESCRIPTOR_HANDLE{};UINT n=range?1:count;if(rtvs&&n<=8)for(UINT i=0;i<n;++i)s->rtvs[i]=rtvs[i];
 }
 void viewport(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* values){std::lock_guard<std::mutex> lock(mutex_);auto s=state(reinterpret_cast<UINT64>(list));if(s&&count<=8){s->vpCount=count;if(count)memcpy(s->vp.data(),values,count*sizeof(*values));}}
 void scissor(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* values){std::lock_guard<std::mutex> lock(mutex_);auto s=state(reinterpret_cast<UINT64>(list));if(s&&count<=8){s->scCount=count;if(count)memcpy(s->sc.data(),values,count*sizeof(*values));}}
 bool duplicate(UINT eye,ID3D12GraphicsCommandList* list,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance,OmFn om,VpFn viewport,ScFn scissor,DrawFn draw){
  if(eye>1||vertices!=3||instances!=1||firstVertex||firstInstance||draws_[eye].load()||(independentConstants_&&!constantsValid_[eye]))return false;
  ListState previous{};{std::lock_guard<std::mutex> lock(mutex_);auto s=state(reinterpret_cast<UINT64>(list));if(!s)return false;previous=*s;}
  if(previous.count!=1||previous.dsv.ptr||!previous.rtvs[0].ptr||previous.vpCount!=1||previous.scCount!=1||previous.vp[0].TopLeftX||previous.vp[0].TopLeftY||previous.vp[0].Width!=static_cast<float>(desktopWidth_)||previous.vp[0].Height!=static_cast<float>(desktopHeight_)||previous.sc[0].left||previous.sc[0].top||previous.sc[0].right!=static_cast<LONG>(desktopWidth_)||previous.sc[0].bottom!=static_cast<LONG>(desktopHeight_))return false;
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={targets_[eye].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET};list->ResourceBarrier(1,&barrier);
  auto rtv=heap_->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=eye*stride_;om(list,1,&rtv,FALSE,nullptr);
  D3D12_VIEWPORT vp{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};D3D12_RECT sc{0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};viewport(list,1,&vp);scissor(list,1,&sc);
  const float clear[]={0,0,0,1};list->ClearRenderTargetView(rtv,clear,0,nullptr);
  if(independentConstants_)list->SetGraphicsRootConstantBufferView(constantRoot_[eye],constantGpu_[eye]);
  draw(list,vertices,instances,firstVertex,firstInstance);
  if(independentConstants_)list->SetGraphicsRootConstantBufferView(constantRoot_[eye],nativeConstantGpu_[eye]);
  om(list,previous.count,previous.rtvs.data(),previous.range,nullptr);viewport(list,previous.vpCount,previous.vp.data());scissor(list,previous.scCount,previous.sc.data());
  std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
  producer_[eye]=reinterpret_cast<UINT64>(list);++draws_[eye];++totalDraws;return true;
 }
 void execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists){for(UINT eye=0;eye<2;++eye){auto producer=producer_[eye].load();if(!producer)continue;for(UINT i=0;i<count;++i)if(reinterpret_cast<UINT64>(lists[i])==producer){executed_[eye]=producer;queue_[eye]=reinterpret_cast<UINT64>(queue);}}}
 HRESULT capture(UINT eye,ID3D12CommandQueue* expectedQueue){
  if(eye>1||draws_[eye].load()!=1||!producer_[eye].load()||executed_[eye].load()!=producer_[eye].load()||queue_[eye].load()!=reinterpret_cast<UINT64>(expectedQueue))return E_UNEXPECTED;
  // Both scene producers and the following XR colour transfer are submitted
  // on this SAME queue. Its submission order provides the dependency. Each
  // eye owns its target, and XR transfer completes before begin() reuses it.
  if(directOutput_){valid_[eye]=true;return S_OK;}
  auto hr=store_.capture(eye,targets_[eye].Get());valid_[eye]=SUCCEEDED(hr);return hr;
 }
 ID3D12Resource* texture(UINT eye){if(eye>1||!valid_[eye])return nullptr;return directOutput_?targets_[eye].Get():store_.texture(eye);}
 ID3D12Resource* privateTarget(UINT eye){return eye<2?targets_[eye].Get():nullptr;}
 bool recordedOn(UINT eye,ID3D12GraphicsCommandList* list)const{return eye<2&&draws_[eye].load()==1&&producer_[eye].load()==reinterpret_cast<UINT64>(list);}
 bool leftRecorded()const{return draws_[0].load()==1;}
};
