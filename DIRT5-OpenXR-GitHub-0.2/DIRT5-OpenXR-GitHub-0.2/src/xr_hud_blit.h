#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <string>

// Diagnostic colour transfer on the game's direct queue. Sources are private
// completed RGBA8 copies in COMMON. OpenXR destinations are acquired/waited
// colour images in RENDER_TARGET and remain there until release.
class XrHudBlit {
 template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
 Ptr<ID3D12Device> device_;Ptr<ID3D12CommandQueue> queue_;
 Ptr<ID3D12CommandAllocator> allocator_;Ptr<ID3D12GraphicsCommandList> list_;
 Ptr<ID3D12RootSignature> root_;Ptr<ID3D12PipelineState> pipeline_;
 Ptr<ID3D12DescriptorHeap> srvs_,rtvs_;Ptr<ID3D12Fence> fence_;
 std::array<Ptr<ID3D12Resource>,3> pendingSources_,pendingDestinations_;
 HANDLE event_=nullptr;UINT64 serial_=0;bool pending_=false;
 UINT srvStride_=0,rtvStride_=0,width_=0,height_=0;
 DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
 static void check(HRESULT h){if(FAILED(h))throw std::runtime_error("Eye blit HRESULT="+std::to_string(h));}
public:
 bool idle()const{return !pending_||(fence_&&fence_->GetCompletedValue()!=UINT64_MAX&&fence_->GetCompletedValue()>=serial_);}
 ~XrHudBlit(){
  // Injection owns this for process lifetime. Do not release pending GPU data.
  if(pending_&&fence_&&fence_->GetCompletedValue()<serial_){
   allocator_.Detach();list_.Detach();root_.Detach();pipeline_.Detach();srvs_.Detach();rtvs_.Detach();fence_.Detach();
   for(auto& x:pendingSources_)x.Detach();for(auto& x:pendingDestinations_)x.Detach();device_.Detach();queue_.Detach();event_=nullptr;
  }
  if(event_)CloseHandle(event_);
 }
 void setup(ID3D12Device* device,ID3D12CommandQueue* queue,UINT width,UINT height,DXGI_FORMAT format){
  if(device_||!width||!height||width>8192||height>8192)throw std::runtime_error("Blit dimensions invalid");
  device_=device;queue_=queue;width_=width;height_=height;format_=format;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_)));
  check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&list_)));check(list_->Close());
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)));event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event_)throw std::runtime_error("Blit event failed");
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=3;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvs_)));srvStride_=device->GetDescriptorHandleIncrementSize(hd.Type);
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs_)));rtvStride_=device->GetDescriptorHandleIncrementSize(hd.Type);
  D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
  D3D12_ROOT_PARAMETER rp{};rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp.DescriptorTable={1,&range};rp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_PARAMETER parameters[2]{rp,{}};parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,1};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
  Ptr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)));
  const char* shader=R"(
Texture2D<float4> source : register(t0);SamplerState clampLinear : register(s0);cbuffer ColourMode:register(b0){uint colourFlags;}
struct V {float4 p:SV_POSITION;float2 uv:TEXCOORD;};
V vs(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 ps(V v):SV_TARGET {float4 pixel=source.SampleLevel(clampLinear,v.uv,0);float3 c=pixel.rgb;
 if(!(colourFlags&2))c=float3(c.r<=0.04045?c.r/12.92:pow((c.r+0.055)/1.055,2.4),c.g<=0.04045?c.g/12.92:pow((c.g+0.055)/1.055,2.4),c.b<=0.04045?c.b/12.92:pow((c.b+0.055)/1.055,2.4));
 if(colourFlags&4)c=float3(c.r<=0.0031308?12.92*c.r:1.055*pow(max(c.r,0),1.0/2.4)-0.055,c.g<=0.0031308?12.92*c.g:1.055*pow(max(c.g,0),1.0/2.4)-0.055,c.b<=0.0031308?12.92*c.b:1.055*pow(max(c.b,0),1.0/2.4)-0.055);
 return float4(c,(colourFlags&1)?pixel.a:1);}
)";
  Ptr<ID3DBlob> vs,ps;check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error));check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root_.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};pd.SampleMask=UINT_MAX;
  pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=format;pd.SampleDesc.Count=1;
  check(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline_)));
 }
 void transfer(const std::array<ID3D12Resource*,3>& sources,const std::array<ID3D12Resource*,3>& destinations,UINT eyeCount=2,bool projectionSourcesLinear=false){
  if(eyeCount<1||eyeCount>3)throw std::runtime_error("Invalid blit image count");
  if(pending_)throw std::runtime_error("Previous eye blit pending");
  for(UINT eye=0;eye<eyeCount;++eye){
   if(!sources[eye]||!destinations[eye])throw std::runtime_error("Blit image missing");
   auto source=sources[eye]->GetDesc(),destination=destinations[eye]->GetDesc();
   if(source.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||source.SampleDesc.Count!=1||destination.Width!=width_||destination.Height!=height_)throw std::runtime_error("Blit resource mismatch");
   D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=source.Format;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;
   auto srv=srvs_->GetCPUDescriptorHandleForHeapStart();srv.ptr+=eye*srvStride_;device_->CreateShaderResourceView(sources[eye],&sd,srv);
   D3D12_RENDER_TARGET_VIEW_DESC vd{};vd.Format=format_;vd.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
   auto rtv=rtvs_->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=eye*rtvStride_;device_->CreateRenderTargetView(destinations[eye],&vd,rtv);
   pendingSources_[eye]=sources[eye];pendingDestinations_[eye]=destinations[eye];
  }
  check(allocator_->Reset());check(list_->Reset(allocator_.Get(),pipeline_.Get()));list_->SetGraphicsRootSignature(root_.Get());ID3D12DescriptorHeap* heaps[]={srvs_.Get()};list_->SetDescriptorHeaps(1,heaps);list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT vp{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};D3D12_RECT sc{0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};list_->RSSetViewports(1,&vp);list_->RSSetScissorRects(1,&sc);
  for(UINT eye=0;eye<eyeCount;++eye){
   D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={sources[eye],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};list_->ResourceBarrier(1,&b);
   auto rtv=rtvs_->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=eye*rtvStride_;auto srv=srvs_->GetGPUDescriptorHandleForHeapStart();srv.ptr+=eye*srvStride_;
   UINT colourFlags=(eye==2?1u:0u)|((projectionSourcesLinear&&eye<2)?2u:0u)|((format_==DXGI_FORMAT_R8G8B8A8_UNORM||format_==DXGI_FORMAT_B8G8R8A8_UNORM)?4u:0u);
   list_->OMSetRenderTargets(1,&rtv,FALSE,nullptr);list_->SetGraphicsRootDescriptorTable(0,srv);list_->SetGraphicsRoot32BitConstant(1,colourFlags,0);list_->DrawInstanced(3,1,0,0);
   std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list_->ResourceBarrier(1,&b);
  }
  check(list_->Close());pending_=true;++serial_;ID3D12CommandList* lists[]={list_.Get()};queue_->ExecuteCommandLists(1,lists);check(queue_->Signal(fence_.Get(),serial_));
  if(fence_->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("Blit device removed");
  if(fence_->GetCompletedValue()<serial_){check(fence_->SetEventOnCompletion(serial_,event_));if(WaitForSingleObject(event_,2000)!=WAIT_OBJECT_0)throw std::runtime_error("Eye blit fence timeout");}
  if(fence_->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("Blit device removed");
  pending_=false;for(auto& x:pendingSources_)x.Reset();for(auto& x:pendingDestinations_)x.Reset();
 }
};
