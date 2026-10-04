#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <algorithm>

// Submit after the validated eye producer and before each native Present.
// Both Presents remain intact. The XR transfer fence completes this SAME
// queue before the next stereo pair reuses these two allocators.
class MonoMonitor {
 template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
 Ptr<ID3D12Device> device_;
 Ptr<ID3D12Resource> scene_,hud_;
 Ptr<ID3D12RootSignature> root_;
 Ptr<ID3D12PipelineState> pipeline_;
 Ptr<ID3D12DescriptorHeap> srv_,rtv_;
 std::array<Ptr<ID3D12CommandAllocator>,2> allocators_;
 std::array<Ptr<ID3D12GraphicsCommandList>,2> lists_;
 UINT rtvStride_=0;
 DXGI_FORMAT outputFormat_=DXGI_FORMAT_UNKNOWN;
public:
 HRESULT setup(ID3D12Device* device,ID3D12Resource* left,ID3D12Resource* hud,DXGI_FORMAT outputFormat){
  if(!device||!left||!hud||(outputFormat!=DXGI_FORMAT_R8G8B8A8_UNORM&&outputFormat!=DXGI_FORMAT_B8G8R8A8_UNORM))return E_INVALIDARG;
  device_=device;scene_=left;hud_=hud;outputFormat_=outputFormat;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  auto hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv_));if(FAILED(hr))return hr;
  auto cpu=srv_->GetCPUDescriptorHandleForHeapStart();UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  for(auto source:{left,hud}){auto desc=source->GetDesc();if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||desc.SampleDesc.Count!=1)return E_INVALIDARG;
   D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(source,&view,cpu);cpu.ptr+=stride;}
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv_));if(FAILED(hr))return hr;rtvStride_=device->GetDescriptorHandleIncrementSize(hd.Type);
  D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=2;
  D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,6};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Ptr<ID3DBlob> signature,error;hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error);if(FAILED(hr))return hr;
  hr=device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root_));if(FAILED(hr))return hr;
  const char* shader=R"(
Texture2D<float4> scene:register(t0); Texture2D<float4> hud:register(t1);
SamplerState clampLinear:register(s0);
cbuffer Mirror:register(b0){float2 scale;float2 offset;uint useHud;uint sourceLinear;}
struct V{float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float3 decode(float3 c){return lerp(c/12.92,pow((max(c,0)+.055)/1.055,2.4),step(.04045,c));}
float3 encode(float3 c){c=saturate(c);return lerp(c*12.92,1.055*pow(c,1.0/2.4)-.055,step(.0031308,c));}
float4 ps(V v):SV_TARGET{float3 c=scene.SampleLevel(clampLinear,v.uv*scale+offset,0).rgb;if(!sourceLinear)c=decode(c);
 if(useHud){float4 h=hud.SampleLevel(clampLinear,v.uv,0);c=decode(h.rgb)+c*(1-saturate(h.a));}return float4(encode(c),1);}
)";
  Ptr<ID3DBlob> vs,ps;hr=D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error);if(FAILED(hr))return hr;
  hr=D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error);if(FAILED(hr))return hr;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root_.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};pd.SampleMask=UINT_MAX;
  pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;
  pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=outputFormat;pd.SampleDesc.Count=1;
  hr=device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline_));if(FAILED(hr))return hr;
  for(UINT eye=0;eye<2;++eye){hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators_[eye]));if(FAILED(hr))return hr;
   hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators_[eye].Get(),nullptr,IID_PPV_ARGS(&lists_[eye]));if(FAILED(hr))return hr;hr=lists_[eye]->Close();if(FAILED(hr))return hr;}
  return S_OK;
 }
 HRESULT submit(ID3D12CommandQueue* queue,ID3D12Resource* backbuffer,UINT eye,bool useHud,bool sourceLinear){
  if(!queue||eye>1)return E_INVALIDARG;auto hr=allocators_[eye]->Reset();if(FAILED(hr))return hr;
  hr=lists_[eye]->Reset(allocators_[eye].Get(),nullptr);if(FAILED(hr))return hr;
  if(!record(lists_[eye].Get(),backbuffer,eye,useHud,sourceLinear))return E_INVALIDARG;
  hr=lists_[eye]->Close();if(FAILED(hr))return hr;ID3D12CommandList* list[]={lists_[eye].Get()};queue->ExecuteCommandLists(1,list);return S_OK;
 }
 bool record(ID3D12GraphicsCommandList* list,ID3D12Resource* backbuffer,UINT eye,bool useHud,bool sourceLinear){
  if(!list||!backbuffer||eye>1||!pipeline_)return false;auto desc=backbuffer->GetDesc();auto source=scene_->GetDesc();
  if(desc.Format!=outputFormat_||!desc.Width||!desc.Height||desc.SampleDesc.Count!=1||!(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))return false;
  // Preserve aspect ratio by cropping the same central part of the left eye.
  float sourceAspect=static_cast<float>(source.Width)/source.Height,outputAspect=static_cast<float>(desc.Width)/desc.Height;
  struct Constants{float scale[2],offset[2];UINT useHud,sourceLinear;} c{{1,1},{0,0},useHud?1u:0u,sourceLinear?1u:0u};
  if(outputAspect>sourceAspect)c.scale[1]=sourceAspect/outputAspect;else c.scale[0]=outputAspect/sourceAspect;
  c.offset[0]=(1-c.scale[0])*.5f;c.offset[1]=(1-c.scale[1])*.5f;
  auto rtv=rtv_->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=eye*rtvStride_;D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=outputFormat_;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;device_->CreateRenderTargetView(backbuffer,&view,rtv);
  std::array<D3D12_RESOURCE_BARRIER,3> barriers{};UINT n=useHud?3:2;
  for(UINT i=0;i<n;++i){auto& b=barriers[i];b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;b.Transition.StateAfter=i==0?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;b.Transition.pResource=i==0?backbuffer:i==1?scene_.Get():hud_.Get();}
  list->ResourceBarrier(n,barriers.data());list->SetPipelineState(pipeline_.Get());list->SetGraphicsRootSignature(root_.Get());ID3D12DescriptorHeap* heaps[]={srv_.Get()};list->SetDescriptorHeaps(1,heaps);
  list->SetGraphicsRootDescriptorTable(0,srv_->GetGPUDescriptorHandleForHeapStart());list->SetGraphicsRoot32BitConstants(1,6,&c,0);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT vp{0,0,static_cast<float>(desc.Width),static_cast<float>(desc.Height),0,1};D3D12_RECT sc{0,0,static_cast<LONG>(desc.Width),static_cast<LONG>(desc.Height)};list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);list->DrawInstanced(3,1,0,0);
  for(UINT i=0;i<n;++i)std::swap(barriers[i].Transition.StateBefore,barriers[i].Transition.StateAfter);list->ResourceBarrier(n,barriers.data());return true;
 }
};
