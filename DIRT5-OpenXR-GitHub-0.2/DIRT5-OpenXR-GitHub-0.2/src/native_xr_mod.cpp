#include <unordered_set>
#include "eye_gpu_store.h"
#include "native_final_eye.h"
#include "mono_monitor.h"
#include "gpu_budget_log.h"
#include "instance_buffer_readback.h"
#include "off_axis_policy.h"
#include "dirt5_camera.h"
#include "xr_hud_runtime.h"
#include <MinHook.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <climits>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
using SetViewFn=void(*)(void*,const dirt5_camera::NativeViewMatrices*);
static dirt5_xr_pair::Runtime* runtime=nullptr;static UINT64 viewAddress=0;static SetViewFn setView;
static dirt5_camera::NativeViewMatrices savedView{};static std::array<dirt5_camera::NativeViewMatrices,2> eyeMatrices{};
static std::array<bool,2> applied{},retained{};static bool viewRestored=false;static std::string pairFailure;
using FrameFn=void(*)(void*,bool,bool);using PrepareFn=void(*)();
using BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
static PresentFn originalPresent;static std::atomic<UINT> presentCount{0};
static std::array<std::atomic<UINT64>,2> presentTicks{},eyeCaptureTicks{};
static std::array<std::atomic<UINT>,2> presentCalls{},presentIntervalMask{};
static std::array<std::atomic<UINT64>,2> presentMaxTicks{};
static std::array<std::atomic<UINT>,2> presentFlagMask{},presentNonOk{};
static FrameFn originalFrame;static PrepareFn prepare;static BarrierFn originalBarrier;static ExecuteFn originalExecute;
static HMODULE self;static UINT64 base=0,cameraAddress=0;static EyeGpuStore* store=nullptr;
static IDXGISwapChain3* gameSwap=nullptr;static std::array<UINT,2> indices{};static std::array<UINT64,2> resources{};
static void logSwapState(std::ofstream& log,const char* phase){
 if(!gameSwap)return;
 DXGI_SWAP_CHAIN_DESC1 desc{};UINT latency=0;BOOL fullscreen=FALSE;
 auto descHr=gameSwap->GetDesc1(&desc),latencyHr=gameSwap->GetMaximumFrameLatency(&latency),fullscreenHr=gameSwap->GetFullscreenState(&fullscreen,nullptr);
 log<<"SWAP_STATE phase="<<phase<<" descHr="<<descHr<<" buffers="<<desc.BufferCount<<" flags="<<desc.Flags<<" effect="<<desc.SwapEffect<<" size="<<desc.Width<<'x'<<desc.Height<<" latencyHr="<<latencyHr<<" maxLatency="<<latency<<" fullscreenHr="<<fullscreenHr<<" fullscreen="<<fullscreen<<std::endl;
}
static bool stereoSwapLatency=false,swapLatencyOwned=false;static UINT savedSwapLatency=0;
static void configureStereoSwapLatency(std::ofstream& log){
 if(!stereoSwapLatency||!gameSwap)return;
 DXGI_SWAP_CHAIN_DESC1 desc{};UINT current=0;
 if(FAILED(gameSwap->GetDesc1(&desc))||!(desc.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)||desc.BufferCount<3||FAILED(gameSwap->GetMaximumFrameLatency(&current)))throw std::runtime_error("Stereo desktop queue configuration unsupported");
 if(current!=1)return;
 if(FAILED(gameSwap->SetMaximumFrameLatency(2)))throw std::runtime_error("Stereo desktop queue setting failed");
 if(!swapLatencyOwned)savedSwapLatency=current;
 swapLatencyOwned=true;log<<"STEREO_SWAP_LATENCY previous="<<current<<" current=2 bothPresentCallsPreserved=1"<<std::endl;
}
static void restoreSwapLatency(std::ofstream& log){
 if(!swapLatencyOwned||!gameSwap)return;UINT current=0;
 auto readHr=gameSwap->GetMaximumFrameLatency(&current);auto restoreHr=SUCCEEDED(readHr)&&current==2?gameSwap->SetMaximumFrameLatency(savedSwapLatency):readHr;
 log<<"SWAP_LATENCY_RESTORE saved="<<savedSwapLatency<<" observed="<<current<<" result="<<restoreHr<<std::endl;
 if(SUCCEEDED(restoreHr))swapLatencyOwned=false;
}
static ID3D12Resource* target=nullptr;static ID3D12CommandQueue* gameQueue=nullptr;
static bool swapOwned=false,queueOwned=false,hooksInitialized=false;
static ComPtr<ID3D12Fence> privateDrainFence;static HANDLE privateDrainEvent=nullptr;static UINT64 privateDrainSerial=0;
static bool drainPrivateQueue(std::ofstream& log){
 if(!gameQueue||!privateDrainFence)return true;
 auto serial=++privateDrainSerial;auto hr=gameQueue->Signal(privateDrainFence.Get(),serial);if(FAILED(hr))return false;
 auto done=privateDrainFence->GetCompletedValue();if(done==UINT64_MAX)return false;
 if(done<serial){hr=privateDrainFence->SetEventOnCompletion(serial,privateDrainEvent);if(FAILED(hr)||WaitForSingleObject(privateDrainEvent,2000)!=WAIT_OBJECT_0){log<<"PRIVATE_GPU_DRAIN_TIMEOUT: resources retained"<<std::endl;return false;}}
 done=privateDrainFence->GetCompletedValue();return done!=UINT64_MAX&&done>=serial;
}
static std::atomic<ID3D12Resource*> observedTarget{nullptr};
static std::atomic<bool> stopRequested{false},captureEnabled{false};static std::atomic<UINT> completedPairs{0};
struct PairRecord{UINT64 counters[3];XrTime displayTime;float leftPosition[3],rightPosition[3];float seat[16],eyeWorld[2][16];};
static std::array<PairRecord,2048> ledger{};static dirt5_camera::StereoHistory history;
static std::atomic<UINT> state{0},pairState{0},active{0},targetState{UINT_MAX};
struct Guard{Guard(){active.fetch_add(1,std::memory_order_acquire);}~Guard(){active.fetch_sub(1,std::memory_order_release);}};
static std::atomic<UINT64> finalList{0},executedList{0},lastQueue{0};static std::atomic<bool> splitPending{false};
static thread_local bool internal=false;
static DerivedCameraFn originalDerived=nullptr;
static std::atomic<bool> preserveOffAxis{false};static std::atomic<UINT> policyOverrides{0};
static std::array<bool,2> fullOpticalCentre{},glassClipAligned{};
static UINT64 shaderParamsAddress=0;
static bool matrixClose(const DirectX::XMFLOAT4X4& a,const DirectX::XMFLOAT4X4& b){for(int i=0;i<4;++i)for(int j=0;j<4;++j)if(!std::isfinite(a.m[i][j])||!std::isfinite(b.m[i][j])||std::abs(a.m[i][j]-b.m[i][j])>0.00002f)return false;return true;}

static std::array<HRESULT,2> captureResults{E_PENDING,E_PENDING};static std::array<bool,2> stateVerified{};
static std::array<UINT64,3> counters{};static std::array<std::array<BYTE,40>,3> contexts{};static std::array<std::array<BYTE,64>,3> cameras{};static std::array<bool,3> readOk{};
static bool read(UINT64,void*,SIZE_T);
using NativeDimensionsFn=void(*)(void*,UINT*,UINT,UINT);
static NativeDimensionsFn originalNativeDimensions=nullptr;
static std::atomic<bool> nativeResolutionOverride{false},nativeRestoreRequested{false},nativeRestoreDone{true};
static std::atomic<UINT> nativeResolutionChanges{0},nativeResolutionRestores{0};
static UINT nativeEyeWidth=0,nativeEyeHeight=0;static bool nativeResizeEnabled=false;static UINT64 nativeWarmupUntil=0;
static bool finalOutputLinear=true;
static bool directFinalOutput=false;
static bool independentFinalConstants=false;static UINT independentConstantWrites=0;
static bool rightMatrixBankSelection=false;static std::atomic<UINT64> rightDescriptorSelections{0},rightRecordSelections{0};
static bool nativeResizePending=false;static UINT nativeWarmup=0;static std::atomic<bool> nativeResolutionWasUsed{false};
static void nativeDimensions(void* options,UINT* output,UINT width,UINT height){
 Guard guard;originalNativeDimensions(options,output,width,height);
 if(reinterpret_cast<UINT64>(options)!=base+0xe6cd30)return;
 if(nativeResolutionOverride.load()){output[0]=output[2]=nativeEyeWidth;output[1]=output[3]=nativeEyeHeight;++nativeResolutionChanges;}
 else if(nativeRestoreRequested.load()){++nativeResolutionRestores;}
}
static void requestNativeRebuild(){BYTE one=1;SIZE_T written=0;if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+0x12f2a0d),&one,1,&written)||written!=1)throw std::runtime_error("Native graphics rebuild request failed");}

static ComPtr<ID3D12Resource> selectedSource;
static std::atomic<UINT> cinemaFrames{0},modeSwitches{0},lastMode{UINT_MAX},currentCameraMode{UINT_MAX};
static std::atomic<bool> requestedRecenter{false},workerBusy{true};
static std::ofstream* liveLog=nullptr;static HANDLE stopEvent=nullptr,recenterEvent=nullptr;
using RootCbvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT64);
static RootCbvFn originalRootCbv=nullptr;
struct UploadPool{ID3D12Resource* resource=nullptr;BYTE* bytes=nullptr;UINT64 address=0,size=0;};
static std::array<UploadPool,2> uploadPools{};
struct RootRecord{UINT root,pool;UINT64 address,list;BYTE bytes[4096];};
static std::array<std::vector<RootRecord>,2> rootRecords;
static std::mutex rootMutex;
using MatrixUploadFn=void(*)(void*,const void*);using BulkFn=void(*)(void*,INT);
using WorldDescFn=void(*)(const void*,const void*,UINT,void*,void*,UINT64,bool);
static WorldDescFn originalWorldDesc=nullptr;static MatrixUploadFn originalMatrixB=nullptr;
static MatrixUploadFn originalMatrixA=nullptr;static BulkFn originalBulk=nullptr;
struct PoseCopy{UINT64 current,previous;std::array<BYTE,64> source,backup;bool patched=false;};
static std::vector<PoseCopy> poseCopies;static std::unordered_set<UINT64> poseSeen;static std::mutex poseMutex;static UINT64 posePatches=0;static UINT maxPosePatches=0;
static std::atomic<UINT64> poseCollectTicks{0},poseCollectCalls{0};static UINT64 leftTicks=0,rightTicks=0,patchTicks=0,restoreTicks=0,submitTicks=0;
static UINT64 nowTicks(){LARGE_INTEGER n{};QueryPerformanceCounter(&n);return n.QuadPart;}
struct PoseTiming{UINT64 start=nowTicks();~PoseTiming(){poseCollectTicks.fetch_add(nowTicks()-start);++poseCollectCalls;}};
struct DrawBinding{UINT tiny[2]{};UINT64 position=0;D3D12_VERTEX_BUFFER_VIEW vertices[8]{};};
static std::unordered_map<UINT64,DrawBinding> drawBindings;
struct DrawRecord{UINT64 list,position,stack[4];UINT tiny[2],args[5],indexed;};
static_assert(sizeof(DrawRecord)==80);
static std::array<std::vector<DrawRecord>,2> drawRecords;
using FlushFn=void(*)(void*);using WorldUiFn=void(*)(void*,UINT,void*,void*);
static FlushFn originalFlush=nullptr;static WorldUiFn originalWorldUi=nullptr;
struct ShaderBinding{UINT64 shader=0,pso=0;char name[128]{};};
static std::unordered_map<UINT64,ShaderBinding> shaderBindings;
struct DrawShaderRecord{DrawRecord draw;ShaderBinding shader;};
static std::array<std::vector<DrawShaderRecord>,2> drawShaderRecords;
static std::atomic<UINT64> worldUiSuppressed{0};
using SetPsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
using DispatchFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
static SetPsoFn originalSetPso=nullptr;static DispatchFn originalDispatch=nullptr;
struct DispatchRecord{UINT64 list,pso,stack[4];UINT groups[3],eye;char name[128];};
static std::vector<DispatchRecord> dispatchRecords;static std::unordered_map<UINT64,UINT64> currentPso;
using SkinRenderFn=void(*)(void*);static SkinRenderFn originalSkinRender=nullptr;
struct SkinRenderRecord{UINT phase,pair,count,afterCount;UINT64 context,counter,stack[4];BYTE globals[128];};
static std::vector<SkinRenderRecord> skinRenderRecords;static std::mutex skinRenderMutex;
struct CharacterVertexRecord{DrawRecord draw;D3D12_VERTEX_BUFFER_VIEW vertices[8];UINT64 shader;UINT eye,reserved;char name[128];};
static std::vector<CharacterVertexRecord> characterVertexRecords;
static std::unordered_map<UINT64,ComPtr<ID3D12PipelineState>> characterPipelines;
using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
using UnmapFn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
static MapFn originalSceneMap=nullptr;static UnmapFn originalSceneUnmap=nullptr;
static ID3D12Resource* sceneConstantResource=nullptr;static void* sceneMapped=nullptr;
struct SceneConstantRecord{UINT eye,pair,stage,reserved;BYTE data[368];};static std::vector<SceneConstantRecord> sceneConstantRecords;static std::mutex sceneConstantMutex;
static std::atomic<UINT64> sceneMapCalls{0},sceneUnmapCalls{0};
static UINT64 sceneShadowAddress=0;
static UINT64 discoveredSceneBinding=0,discoveredSceneCb=0;
using UniformWriteFn=void*(*)(void*,const void*,UINT);static UniformWriteFn originalUniformWrite=nullptr;
static UINT64 raceSceneCb=0;static std::atomic<UINT> sceneAnimationBank{UINT_MAX};static std::atomic<UINT64> animationBankOverrides{0};
struct ResolutionUniformRecord{UINT phase,width,height,pair,eye,bytes;UINT64 buffer;char name[64];BYTE data[3248];};
static bool parameterAudit=false;static std::vector<ResolutionUniformRecord> resolutionUniformRecords;static std::mutex resolutionUniformMutex;
static std::unordered_map<std::string,UINT> resolutionUniformCounts;
static void auditResolutionUniform(void* buffer,const void* data,UINT bytes){
 if(!parameterAudit||(bytes!=128&&bytes!=368&&bytes!=3248))return;
 ResolutionUniformRecord record{};record.bytes=bytes;record.buffer=reinterpret_cast<UINT64>(buffer);
 if(!read(record.buffer+0xb8,record.name,63)||!record.name[0])return;
 for(UINT i=0;i<63&&record.name[i];++i)if(record.name[i]<32||record.name[i]>126)return;
 record.phase=nativeResolutionOverride.load()?(captureEnabled.load()?2:1):(nativeResolutionRestores.load()?3:0);
 record.pair=completedPairs.load();record.eye=presentCount.load();
 UINT64 cfg=0;if(read(base+0x12f2a18,&cfg,8)&&cfg)read(cfg+0x3d0,&record.width,8);
 std::lock_guard<std::mutex> lock(resolutionUniformMutex);
 auto key=std::to_string(record.phase)+":"+record.name+":"+std::to_string(record.buffer)+":"+std::to_string(record.eye)+":"+std::to_string(record.width)+"x"+std::to_string(record.height);
 if(resolutionUniformRecords.size()>=1024||resolutionUniformCounts[key]>=1)return;
 if(read(reinterpret_cast<UINT64>(data),record.data,bytes)){++resolutionUniformCounts[key];resolutionUniformRecords.push_back(record);}
}
static void* writeUniform(void* buffer,const void* data,UINT bytes){
 Guard guard;
 auditResolutionUniform(buffer,data,bytes);
 if(captureEnabled.load()&&pairState.load()==1&&currentCameraMode.load()==8&&reinterpret_cast<UINT64>(buffer)==raceSceneCb&&bytes==368){
  std::array<BYTE,368> copy{};
  if(read(reinterpret_cast<UINT64>(data),copy.data(),copy.size())){
   UINT selected=UINT_MAX;memcpy(&selected,copy.data()+360,4);
   if(selected<=1){
    UINT eye=presentCount.load();if(eye==0)sceneAnimationBank=selected;
    else if(eye==1&&sceneAnimationBank.load()<=1){UINT left=sceneAnimationBank.load();if(left!=selected){memcpy(copy.data()+360,&left,4);++animationBankOverrides;}return originalUniformWrite(buffer,copy.data(),bytes);}
   }
  }
 }
 return originalUniformWrite(buffer,data,bytes);
}
static void locateSceneShadow(UINT64 context){
 if(sceneShadowAddress)return;UINT64 binding=0,metadata=0;UINT id=UINT_MAX,count=0,size=0;char name[64]{};
 if(!read(context+0x548,&binding,8)||!read(binding,&metadata,8)||!read(metadata+0xc08,&id,4)||id!=1||!read(binding+0x18,&count,4)||count>256)return;
 auto cb=binding+0x90+static_cast<UINT64>(count)*16;
 if(!read(cb+0x10,&size,4)||size!=368||!read(cb+0xb8,name,64)||strcmp(name,"gooSceneParamsCB"))return;
 MEMORY_BASIC_INFORMATION owner{};if(!VirtualQuery(reinterpret_cast<void*>(binding),&owner,sizeof(owner))||owner.State!=MEM_COMMIT||owner.Protect!=PAGE_READWRITE)return;
 // VirtualQuery(binding).BaseAddress can start at the queried page, after the
 // shadow allocation. Validate the preceding pages in the same allocation.
 UINT64 first=binding>65536?binding-65536:UINT64(0);first=(first+7)&~UINT64(7);
 std::vector<UINT64> matches;
 for(UINT64 cursor=first;cursor<binding;){
  MEMORY_BASIC_INFORMATION region{};if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof(region)))break;
  UINT64 end=std::min(binding,reinterpret_cast<UINT64>(region.BaseAddress)+region.RegionSize);
  if(end<=cursor)break;
  if(region.AllocationBase==owner.AllocationBase&&region.State==MEM_COMMIT&&region.Protect==PAGE_READWRITE){
   std::vector<BYTE> bytes(static_cast<SIZE_T>(end-cursor));
   if(read(cursor,bytes.data(),bytes.size()))for(SIZE_T offset=0;offset+16<=bytes.size();offset+=8){UINT64 values[2]{};memcpy(values,bytes.data()+offset,16);if(values[0]!=binding||values[1]!=cb)continue;UINT selector=UINT_MAX;if(read(cursor+offset+376,&selector,4)&&selector<=1)matches.push_back(cursor+offset);}
  }
  cursor=end;
 }
 if(matches.size()==1){sceneShadowAddress=matches[0];discoveredSceneBinding=binding;discoveredSceneCb=cb;}

}
static void recordSceneShadow(UINT stage){return;if(!sceneShadowAddress)return;SceneConstantRecord record{};record.eye=presentCount.load();record.pair=completedPairs.load();record.stage=stage;if(read(sceneShadowAddress+16,record.data,sizeof(record.data))){std::lock_guard<std::mutex> lock(sceneConstantMutex);if(sceneConstantRecords.size()<128)sceneConstantRecords.push_back(record);}}
struct DriverParameterRecord{UINT eye,slot,id,root,count,reserved;UINT64 context,metadata,binding;BYTE descriptor[0xcc0],bound[512];};
static std::vector<DriverParameterRecord> driverParameters;static std::array<bool,2> driverParamsSeen{};
static HRESULT STDMETHODCALLTYPE sceneMap(ID3D12Resource* resource,UINT subresource,const D3D12_RANGE* range,void** pointer){Guard guard;auto hr=originalSceneMap(resource,subresource,range,pointer);if(resource==sceneConstantResource&&SUCCEEDED(hr)&&pointer){std::lock_guard<std::mutex> lock(sceneConstantMutex);sceneMapped=*pointer;++sceneMapCalls;}return hr;}
static void STDMETHODCALLTYPE sceneUnmap(ID3D12Resource* resource,UINT subresource,const D3D12_RANGE* range){Guard guard;if(resource==sceneConstantResource){std::lock_guard<std::mutex> lock(sceneConstantMutex);++sceneUnmapCalls;if(sceneMapped&&sceneConstantRecords.size()<128){SceneConstantRecord record{};record.eye=pairState.load()==1?presentCount.load():2;record.pair=completedPairs.load();record.stage=0;if(read(reinterpret_cast<UINT64>(sceneMapped),record.data,sizeof(record.data)))sceneConstantRecords.push_back(record);}sceneMapped=nullptr;}originalSceneUnmap(resource,subresource,range);}
using DrawFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
using DrawIndexedFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
using VertexFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*);
using SingleConstantFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
static DrawFn originalDraw=nullptr;static DrawIndexedFn originalDrawIndexed=nullptr;static VertexFn originalVertex=nullptr;static SingleConstantFn originalSingleConstant=nullptr;

static std::atomic<bool> auditThisPair{false};static bool movingCaptured=false;static std::array<float,3> auditOrigin{};static PairRecord capturedPair{};
struct WorldInputRecord{UINT index,metadata;BYTE source[64],current[64],previous[64];};static_assert(sizeof(WorldInputRecord)==200);
static std::array<std::vector<WorldInputRecord>,2> worldInputs;
static InstanceBufferReadback* instanceReader=nullptr;static ID3D12Resource* instanceResource=nullptr;
static std::array<UINT64,2> boneWrappers{};
static std::array<ID3D12Resource*,2> boneResources{};
static std::array<InstanceBufferReadback*,2> boneReaders{};
static std::array<std::array<std::vector<BYTE>,2>,2> boneBytes;
static std::array<std::atomic<UINT>,2> boneStates{64,64};
static std::array<std::atomic<bool>,2> boneSplit{false,false};
static std::array<std::vector<BYTE>,2> uniformBytes;
static ComPtr<ID3D12Resource> uniformResource;
static InstanceBufferReadback* uniformReader=nullptr;static std::atomic<UINT> uniformState{UINT_MAX};static std::atomic<bool> uniformSplit{false};
static ID3D12Resource* driverResource=nullptr;static InstanceBufferReadback* driverReader=nullptr;static UINT64 driverWrapper=0;static std::atomic<UINT> driverState{64};static std::atomic<bool> driverSplit{false};
static std::array<std::vector<BYTE>,2> driverBytes;
static UINT64 instanceWrapper=0;static std::atomic<UINT> instanceState{64};static std::atomic<bool> instanceSplit{false};
static std::array<std::vector<BYTE>,2> instanceBytes,cpuInstanceRecords;

using ConstantsFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const void*,UINT);
static ConstantsFn originalConstants=nullptr;
struct ConstantsRecord{UINT root,count,offset,reserved;UINT64 list;UINT values[16];};
static std::array<std::vector<ConstantsRecord>,2> constantsRecords;
using UploadAddressFn=void(*)(void*);static UploadAddressFn originalUploadAddress=nullptr;
struct ContextRecord{UINT64 address;BYTE bytes[1024];};
static std::vector<ContextRecord> uploadContexts;

static std::array<std::array<BYTE,128>,3> worldGlobals{};
static std::array<std::array<BYTE,3248>,2> fullShaderViews{};
static UINT64 previousCamera=0;static bool replay=false;
static UINT64 currentSceneCb(){
 UINT64 binding=0;UINT count=0,size=0;char name[64]{};
 if(!read(base+0x12f1b30,&binding,8)||!binding||!read(binding+0x18,&count,4)||count>256)return 0;
 UINT64 cb=binding+0x90+static_cast<UINT64>(count)*16;
 if(!read(cb+0x10,&size,4)||size!=368||!read(cb+0xb8,name,64)||strcmp(name,"gooSceneParamsCB"))return 0;
 return cb;
}
static bool activeCamera(){
 UINT count=0;UINT64 record=0,handle=0,cam=0,view=0,vt=0,table=0,method=0,manager=0;UINT mode=UINT_MAX;
 if(!read(base+0x20a88c0,&count,4)||count!=1||!read(base+0x20a88c8,&record,8)||!read(record+0x10,&handle,8)||!read(record+0x20,&cam,8)||!read(handle,&view,8)||!read(cam,&vt,8)||vt!=base+0xcd9760||!read(view,&table,8)||table!=base+0xc647d8||!read(table+32,&method,8)||method!=base+0x1b1760||!read(view+0x25bb0,&manager,8)||!manager||!read(cam+0x1800,&mode,4)||mode>15){currentCameraMode=UINT_MAX;return false;}
 viewAddress=view;cameraAddress=cam+0xc70;shaderParamsAddress=manager+16;setView=reinterpret_cast<SetViewFn>(method);currentCameraMode=mode;
 if(cam!=previousCamera){history={};runtime->recenter();previousCamera=cam;}return mode==8;
}
static UINT width=0,height=0;static DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
static bool read(UINT64 a,void* b,SIZE_T n){SIZE_T got=0;return a&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(a),b,n,&got)&&got==n;}
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("HRESULT="+std::to_string(hr));}
static std::vector<MEMORY_BASIC_INFORMATION> poseRegions;static std::mutex poseRegionMutex;
static bool poseMemory(UINT64 address,MEMORY_BASIC_INFORMATION& info){
 if(!address||(address&15))return false;
 std::lock_guard<std::mutex> lock(poseRegionMutex);
 for(const auto& region:poseRegions){auto begin=reinterpret_cast<UINT64>(region.BaseAddress);if(address>=begin&&address-begin<=region.RegionSize&&region.RegionSize-(address-begin)>=64){info=region;return true;}}

 if(!address||(address&15)||!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)))return false;
 auto begin=reinterpret_cast<UINT64>(info.BaseAddress);bool valid=info.State==MEM_COMMIT&&info.Type==MEM_PRIVATE&&info.Protect==PAGE_READWRITE&&address>=begin&&address-begin<=info.RegionSize&&info.RegionSize-(address-begin)>=64;if(valid&&poseRegions.size()<64)poseRegions.push_back(info);return valid;
}
static void rememberPose(const void* source){
 if(internal||!captureEnabled.load()||pairState.load()!=1||presentCount.load()!=0)return;
 PoseTiming timing;BYTE record[64]{};memcpy(record,source,64);

 PoseCopy pose{};memcpy(&pose.current,record+8,8);memcpy(&pose.previous,record+16,8);if(pose.current==pose.previous)return;
 {std::lock_guard<std::mutex> lock(poseMutex);if(!poseSeen.insert(pose.previous).second)return;}
 MEMORY_BASIC_INFORMATION a{},b{};if(!poseMemory(pose.current,a)||!poseMemory(pose.previous,b)||a.AllocationBase!=b.AllocationBase)return;memcpy(pose.source.data(),reinterpret_cast<void*>(pose.current),64);std::array<BYTE,64> previous{};memcpy(previous.data(),reinterpret_cast<void*>(pose.previous),64);if(previous==pose.source)return;
 float matrix[16];memcpy(matrix,pose.source.data(),64);for(float value:matrix)if(!std::isfinite(value))return;
 if(std::abs(matrix[3])>.0001f||std::abs(matrix[7])>.0001f||std::abs(matrix[11])>.0001f)return;
 std::lock_guard<std::mutex> lock(poseMutex);if(poseCopies.size()<4096)poseCopies.push_back(pose);
}
static void rememberDesc(const void* source){
 if(!captureEnabled.load()||pairState.load()!=1||presentCount.load()!=0)return;
 BYTE record[64]{};UINT64 current=0;INT relative=0;
 memcpy(&current,source,8);memcpy(&relative,reinterpret_cast<const BYTE*>(source)+8,4);
 UINT64 previous=current+static_cast<INT64>(relative);memcpy(record+8,&current,8);memcpy(record+16,&previous,8);rememberPose(record);
}
static bool selectRightDescriptor(const void*,std::array<BYTE,32>&);
static bool selectRightRecord(const void*,std::array<BYTE,64>&);
static void worldDesc(const void* source,const void* primitives,UINT count,void* callback,void* context,UINT64 bits,bool flag){
 Guard guard;std::array<BYTE,32> selected{};auto input=selectRightDescriptor(source,selected)?selected.data():source;originalWorldDesc(input,primitives,count,callback,context,bits,flag);rememberDesc(source);
}
static void matrixB(void* destination,const void* source){Guard guard;std::array<BYTE,32> selected{};auto input=selectRightDescriptor(source,selected)?selected.data():source;originalMatrixB(destination,input);rememberDesc(source);}
struct CompleteBank{UINT64 header,owner,low,high;UINT bytes;};
struct ActorCpuHeader{UINT pair,eye,count,stage;UINT64 counter;float seat[16];};static_assert(sizeof(ActorCpuHeader)==88);
struct ActorCpuRecord{WorldInputRecord input;UINT64 header,owner,bankCurrent,bankPrevious;UINT bones,slot;};static_assert(sizeof(ActorCpuRecord)==240);
struct ActorCpuSnapshot{ActorCpuHeader header{};std::vector<ActorCpuRecord> records;};
static bool actorCpuProbe=false,actorProbeOriginValid=false,actorProbeMoved=false;static int actorProbeStage=-1;
static DirectX::XMFLOAT3 actorProbeOrigin{};static std::array<ActorCpuSnapshot,6> actorCpuSnapshots;
static std::vector<CompleteBank> completeBanks;static std::vector<std::array<UINT64,2>> bankMisses;
static std::unordered_set<UINT64> rightValidatedHeaders;
static UINT64 fullBankPairs=0,fullBankMatrices=0;static std::vector<CompleteBank> firstBanks;
static bool findBank(UINT64 current,UINT64 previous,CompleteBank& bank){
 UINT64 low=std::min(current,previous),delta=std::max(current,previous)-low;
 if(delta<64||delta>0x40000||(delta&63)||low<delta+0x200)return false;
 for(const auto& known:completeBanks)if(known.bytes==delta&&low>=known.low&&low-known.low<delta){bank=known;return true;}
 for(const auto& miss:bankMisses)if(miss[0]==current&&miss[1]==previous)return false;
 auto start=low-delta-0x200;std::vector<BYTE> bytes(static_cast<SIZE_T>(delta+0x200));
 if(read(start,bytes.data(),bytes.size()))for(SIZE_T offset=0;offset+48<=bytes.size();offset+=8){
  UINT64 values[5];UINT count=0,index=0;memcpy(values,bytes.data()+offset,40);memcpy(&count,bytes.data()+offset+40,4);memcpy(&index,bytes.data()+offset+44,4);
  auto first=std::min(values[1],values[2]),second=std::max(values[1],values[2]);
  if(second-first!=delta||static_cast<UINT64>(count)*64!=delta||index>1||first!=start+offset+0x130||low<first||low-first>=delta)continue;
  if(!((values[3]==first&&values[4]==second)||(values[3]==second&&values[4]==first)))continue;
  UINT64 vt=0;if(!read(values[0],&vt,8)||vt!=base+0xc644f0)continue;
  MEMORY_BASIC_INFORMATION a{},b{};
  if(!poseMemory(first,a)||!poseMemory(second,b)||a.AllocationBase!=b.AllocationBase)continue;
  if(first+delta-reinterpret_cast<UINT64>(a.BaseAddress)>a.RegionSize||second+delta-reinterpret_cast<UINT64>(b.BaseAddress)>b.RegionSize)continue;
  bank={start+offset,values[0],first,second,static_cast<UINT>(delta)};completeBanks.push_back(bank);return true;
 }
 if(bankMisses.size()<4096)bankMisses.push_back({current,previous});return false;
}
static bool rightSelectionScope(){return rightMatrixBankSelection&&!internal&&captureEnabled.load()&&pairState.load()==1&&currentCameraMode.load()==8&&presentCount.load()==1;}
static bool validatedRightBank(UINT64 current,UINT64 previous){
 if(!current||!previous||current==previous)return false;
 std::lock_guard<std::mutex> lock(poseMutex);CompleteBank bank{};
 if(!findBank(current,previous,bank))return false;
 auto low=std::min(current,previous);if(low<bank.low||low-bank.low>=bank.bytes||((low-bank.low)&63))return false;
 if(rightValidatedHeaders.count(bank.header))return true;
 UINT64 values[5]{},vt=0;UINT count=0;
 if(!read(bank.header,values,40)||!read(bank.header+40,&count,4)||!read(bank.owner,&vt,8)||vt!=base+0xc644f0||values[0]!=bank.owner||std::min(values[1],values[2])!=bank.low||std::max(values[1],values[2])!=bank.high||static_cast<UINT64>(count)*64!=bank.bytes)return false;
 rightValidatedHeaders.insert(bank.header);
 return true;
}
static bool selectRightDescriptor(const void* source,std::array<BYTE,32>& selected){
 if(!rightSelectionScope()||!read(reinterpret_cast<UINT64>(source),selected.data(),selected.size()))return false;
 UINT64 current=0;INT relative=0;memcpy(&current,selected.data(),8);memcpy(&relative,selected.data()+8,4);UINT64 previous=current+static_cast<INT64>(relative);
 if(!validatedRightBank(current,previous))return false;
 // prepare() advances the native matrix-bank choice between eye renders.
 // The descriptor's previous bank still holds this game's latest scene pose,
 // including actors culled from the left eye. Clone only the input descriptor;
 // do not overwrite either animation bank or rewind native frame/fence state.
 relative=0;memcpy(selected.data(),&previous,8);memcpy(selected.data()+8,&relative,4);++rightDescriptorSelections;return true;
}
static bool selectRightRecord(const void* source,std::array<BYTE,64>& selected){
 if(!rightSelectionScope()||!read(reinterpret_cast<UINT64>(source),selected.data(),selected.size()))return false;
 UINT64 current=0,previous=0;memcpy(&current,selected.data()+8,8);memcpy(&previous,selected.data()+16,8);
 if(!validatedRightBank(current,previous))return false;
 memcpy(selected.data()+8,&previous,8);++rightRecordSelections;return true;
}
static void captureActorCpu(UINT eye){
 if(!actorCpuProbe||actorProbeStage<0||actorProbeStage>2||eye>1)return;
 auto& snapshot=actorCpuSnapshots[actorProbeStage*2+eye];if(!snapshot.records.empty())return;
 UINT64 source=0;UINT count=0,capacity=0;
 if(!read(base+0x12ca870,&source,8)||!source||!read(base+0x12d6104,&count,4)||!read(base+0x12ca878,&capacity,4)||!count||count>capacity||count>16384)return;
 std::vector<BYTE> inputs(static_cast<SIZE_T>(count)*64);if(!read(source,inputs.data(),inputs.size()))return;
 snapshot.header.pair=completedPairs.load();snapshot.header.eye=eye;snapshot.header.stage=actorProbeStage;read(base+0x10da568,&snapshot.header.counter,8);memcpy(snapshot.header.seat,&savedView.currentWorld,64);
 snapshot.records.reserve(count);
 for(UINT i=0;i<count;++i){ActorCpuRecord record{};record.input.index=i;memcpy(record.input.source,inputs.data()+static_cast<SIZE_T>(i)*64,64);memcpy(&record.input.metadata,record.input.source+52,4);UINT64 current=0,previous=0;memcpy(&current,record.input.source+8,8);memcpy(&previous,record.input.source+16,8);
  if(!read(current,record.input.current,64)||!read(previous,record.input.previous,64))continue;
  CompleteBank bank{};bool found=findBank(current,previous,bank);if(!found&&current==previous)for(const auto& known:completeBanks)if((current>=known.low&&current-known.low<known.bytes)||(current>=known.high&&current-known.high<known.bytes)){bank=known;found=true;break;}if(found){record.header=bank.header;record.owner=bank.owner;record.bones=bank.bytes/64;record.slot=static_cast<UINT>((current-(current>=bank.high?bank.high:bank.low))/64);read(bank.header+24,&record.bankCurrent,8);read(bank.header+32,&record.bankPrevious,8);}
  snapshot.records.push_back(record);
 }
 snapshot.header.count=static_cast<UINT>(snapshot.records.size());
}
static void collectCompleteBanks(){
 // Run after first eye workers have joined. The descriptor proves both bank bounds;
 // no range is inferred from a visible bone alone.
 const auto originals=poseCopies;std::unordered_set<UINT64> used;
 for(const auto& pose:originals){CompleteBank bank{};if(!findBank(pose.current,pose.previous,bank)||!used.insert(bank.header).second)continue;
  UINT64 current=(pose.current>=bank.high)?bank.high:bank.low,previous=(current==bank.low)?bank.high:bank.low;
  UINT64 header[5]{};UINT count=0;UINT64 vt=0;
  if(!read(bank.header,header,40)||!read(bank.header+40,&count,4)||!read(bank.owner,&vt,8)||vt!=base+0xc644f0||header[0]!=bank.owner||std::min(header[1],header[2])!=bank.low||std::max(header[1],header[2])!=bank.high||static_cast<UINT64>(count)*64!=bank.bytes)continue;
  std::vector<BYTE> snapshot(bank.bytes);if(!read(current,snapshot.data(),snapshot.size()))continue;
  bool valid=true;for(UINT offset=0;offset<bank.bytes;offset+=64){float m[16];memcpy(m,snapshot.data()+offset,64);for(float v:m)if(!std::isfinite(v))valid=false;for(UINT i:{3u,7u,11u})if(std::abs(m[i])>.0001f)valid=false;}
  if(!valid)continue;
  ++fullBankPairs;fullBankMatrices+=count;if(completedPairs.load()==0)firstBanks.push_back(bank);
  for(UINT offset=0;offset<bank.bytes;offset+=64){if(!poseSeen.insert(previous+offset).second)continue;PoseCopy copy{};copy.current=current+offset;copy.previous=previous+offset;memcpy(copy.source.data(),snapshot.data()+offset,64);if(poseCopies.size()<16384)poseCopies.push_back(copy);}
 }
}
static void matrixA(void* destination,const void* source){Guard guard;std::array<BYTE,64> selected{};auto input=selectRightRecord(source,selected)?selected.data():source;originalMatrixA(destination,input);rememberPose(source);}
static void bulkMatrix(void* ctx,INT index){
 Guard guard;originalBulk(ctx,index);if(index<0||index>32768||!captureEnabled.load()||pairState.load()!=1||presentCount.load()!=0)return;
 UINT64 batchIds=0,batches=0,sourceArray=0,drawList=0;UINT batch=0;UINT16 range[2]{};
 if(!read(reinterpret_cast<UINT64>(ctx),&batchIds,8)||!read(batchIds+static_cast<UINT64>(index)*4,&batch,4)||batch>=98304||!read(base+0x12ca880,&batches,8)||!read(batches+batch*8,range,4)||range[1]>16384||!read(base+0x12ca870,&sourceArray,8)||!read(reinterpret_cast<UINT64>(ctx)+0x18,&drawList,8))return;
 for(UINT i=0;i<range[1];++i){UINT sourceIndex=0;memcpy(&sourceIndex,reinterpret_cast<void*>(drawList+(range[0]+i)*16),4);if(sourceIndex>=98304)break;rememberPose(reinterpret_cast<void*>(sourceArray+static_cast<UINT64>(sourceIndex)*64));}
}
static void patchPoses(){
 std::lock_guard<std::mutex> lock(poseMutex);collectCompleteBanks();UINT patched=0;
 for(auto& pose:poseCopies){if(memcmp(reinterpret_cast<void*>(pose.current),pose.source.data(),64))continue;memcpy(pose.backup.data(),reinterpret_cast<void*>(pose.previous),64);if(pose.backup==pose.source)continue;
 memcpy(reinterpret_cast<void*>(pose.previous),pose.source.data(),64);pose.patched=true;++patched;}
 posePatches+=patched;maxPosePatches=std::max(maxPosePatches,patched);
}
static void restorePoses(){
 std::lock_guard<std::mutex> lock(poseMutex);
 for(auto& pose:poseCopies)if(pose.patched){if(!memcmp(reinterpret_cast<void*>(pose.previous),pose.source.data(),64))memcpy(reinterpret_cast<void*>(pose.previous),pose.backup.data(),64);pose.patched=false;}
}
using UiPassFn=void(*)(void*,void*,void*);using UiTargetsFn=void(*)(void*,const void*,bool);
static UiPassFn originalUiPass=nullptr;static UiTargetsFn originalUiTargets=nullptr;
static ComPtr<ID3D12Resource> hudCopy;static EyeGpuStore* hudReader=nullptr;static bool hudArmed=false,hudCopied=false;static UINT hudClears=0;static UINT64 hudCopiedFrames=0;static HRESULT hudCaptureResult=E_PENDING;
static thread_local bool withinUi=false;static std::atomic<ID3D12Resource*> uiSurface{nullptr};static std::atomic<UINT> uiSurfaceState{UINT_MAX};static std::array<UINT,4> uiStates{};static std::array<UINT,2> uiDesc{};static UINT uiWidth=0,uiHeight=0;static std::array<void*,15> resourceMethods{};
struct UiRecord{UINT eye,stage;UINT64 a,b,c,wrapper,list;BYTE args[3][256],targets[64],command[1408];};
static std::vector<UiRecord> uiRecords;static std::mutex uiMutex;
static void saveUi(UINT stage,void* a,void* b,void* c){return;
 if(!captureEnabled.load()||pairState.load()!=1||completedPairs.load()>1||presentCount.load()>1)return;
 UiRecord record{};record.eye=presentCount.load();record.stage=stage;record.a=reinterpret_cast<UINT64>(a);record.b=reinterpret_cast<UINT64>(b);record.c=reinterpret_cast<UINT64>(c);
 UINT64 addresses[]={record.a,record.b,record.c};for(UINT i=0;i<3;++i)read(addresses[i],record.args[i],256);
 read(record.a,record.targets,64);read(record.a+0x40,&record.wrapper,8);read(record.wrapper+0x30,&record.list,8);read(record.wrapper,record.command,sizeof(record.command));
 std::lock_guard<std::mutex> lock(uiMutex);if(uiRecords.size()<128)uiRecords.push_back(record);
}
static void uiPass(void* a,void* b,void* c){Guard guard;
 if(captureEnabled.load()&&pairState.load()==1&&presentCount.load()<2){UINT64 native=0,resource=0,vt=0;std::array<void*,15> methods{};
  if(read(reinterpret_cast<UINT64>(a)+8,&native,8)&&read(native+0xb0,&resource,8)&&read(resource,&vt,8)&&read(vt,methods.data(),sizeof(methods))){
   bool verified=true;for(UINT slot:{0u,1u,2u,7u,10u})if(methods[slot]!=resourceMethods[slot])verified=false;
   if(verified){auto surface=reinterpret_cast<ID3D12Resource*>(resource);if(uiSurface.exchange(surface)!=surface)uiSurfaceState=UINT_MAX;auto desc=surface->GetDesc();uiWidth=static_cast<UINT>(desc.Width);uiHeight=desc.Height;uiDesc[0]=desc.Format;uiDesc[1]=desc.Flags;}
  }
  uiStates[presentCount.load()*2]=uiSurfaceState.load();
 }
 saveUi(0,a,b,c);withinUi=true;originalUiPass(a,b,c);withinUi=false;saveUi(1,a,b,c);
 if(captureEnabled.load()&&pairState.load()==1&&presentCount.load()<2)uiStates[presentCount.load()*2+1]=uiSurfaceState.load();
 if(!hudArmed||!captureEnabled.load()||pairState.load()!=1||presentCount.load()>1||currentCameraMode.load()!=8)return;
 auto surface=uiSurface.load();UINT64 native=0,wrapper=0,listAddress=0,heap=0,cpuBase=0;UINT stride=0,rtvIndex=0;
 if(!surface||uiSurfaceState.load()!=D3D12_RESOURCE_STATE_RENDER_TARGET||uiWidth!=width||uiHeight!=height||uiDesc[0]!=DXGI_FORMAT_R8G8B8A8_TYPELESS||
 !read(reinterpret_cast<UINT64>(a)+8,&native,8)||!read(native+0xdc,&rtvIndex,4)||!read(reinterpret_cast<UINT64>(a)+0x40,&wrapper,8)||!read(wrapper+0x30,&listAddress,8)||!read(base+0x10da590,&heap,8)||!read(heap+0x28,&stride,4)||!read(heap+0x30,&cpuBase,8)||!stride||rtvIndex>65535){stopRequested=true;return;}
 auto list=reinterpret_cast<ID3D12GraphicsCommandList*>(listAddress);if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT){stopRequested=true;return;}
 internal=true;
 if(presentCount.load()==0){
  D3D12_RESOURCE_BARRIER barriers[2]{};for(auto& barrier:barriers){barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
  barriers[0].Transition={surface,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
  barriers[1].Transition={hudCopy.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST};
  list->ResourceBarrier(2,barriers);list->CopyResource(hudCopy.Get(),surface);for(auto& barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(2,barriers);hudCopied=true;++hudCopiedFrames;
 }
 D3D12_CPU_DESCRIPTOR_HANDLE rtv{cpuBase+static_cast<UINT64>(rtvIndex)*stride};const float transparent[4]={0,0,0,0};list->ClearRenderTargetView(rtv,transparent,0,nullptr);++hudClears;
 internal=false;
}
static void uiTargets(void* a,const void* b,bool depth){Guard guard;originalUiTargets(a,b,depth);if(withinUi)saveUi(2,a,const_cast<void*>(b),nullptr);}
static bool auditDraw(){return !internal&&captureEnabled.load()&&pairState.load()==1&&auditThisPair.load()&&presentCount.load()<2;}
static bool auditDispatch(){return !internal&&state.load()==1&&(auditDraw()||completedPairs.load()<4);}
static void skinRender(void* context){
 Guard guard;SkinRenderRecord record{};bool recordCall=state.load()==1&&completedPairs.load()<4;
 if(recordCall){record.phase=pairState.load()==1?presentCount.load():2;record.pair=completedPairs.load();record.context=reinterpret_cast<UINT64>(context);read(base+0x12f1ac0,&record.count,4);read(base+0x10da568,&record.counter,8);read(base+0x12f1a90,record.globals,128);void* stack[4]{};USHORT n=CaptureStackBackTrace(1,4,stack,nullptr);for(USHORT i=0;i<n;++i)record.stack[i]=reinterpret_cast<UINT64>(stack[i]);}
 originalSkinRender(context);
 if(recordCall){read(base+0x12f1ac0,&record.afterCount,4);std::lock_guard<std::mutex> lock(skinRenderMutex);if(skinRenderRecords.size()<128)skinRenderRecords.push_back(record);}
}
static void STDMETHODCALLTYPE setPipeline(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso){
 Guard guard;originalSetPso(list,pso);if(!auditDispatch())return;std::lock_guard<std::mutex> lock(rootMutex);currentPso[reinterpret_cast<UINT64>(list)]=reinterpret_cast<UINT64>(pso);
}
static void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z){
 Guard guard;if(auditDispatch()){
  DispatchRecord record{};record.list=reinterpret_cast<UINT64>(list);record.groups[0]=x;record.groups[1]=y;record.groups[2]=z;record.eye=pairState.load()==1?presentCount.load():2;void* stack[4]{};USHORT count=CaptureStackBackTrace(1,4,stack,nullptr);for(USHORT i=0;i<count;++i)record.stack[i]=reinterpret_cast<UINT64>(stack[i]);
  std::lock_guard<std::mutex> lock(rootMutex);record.pso=currentPso[record.list];const auto& binding=shaderBindings[record.list];if(record.pso==binding.pso)memcpy(record.name,binding.name,128);if(dispatchRecords.size()<4096)dispatchRecords.push_back(record);
 }
 originalDispatch(list,x,y,z);
}
static void flush(void* context){
 Guard guard;if(auditDraw()){locateSceneShadow(reinterpret_cast<UINT64>(context));}originalFlush(context);if(!auditDraw())return;
 UINT64 list=0;ShaderBinding binding{};auto address=reinterpret_cast<UINT64>(context);
 if(!read(address+0x30,&list,8)||!list||!read(address+0x530,&binding.shader,8)||!binding.shader)return;
 read(binding.shader+0x168,&binding.pso,8);read(binding.shader,binding.name,127);binding.name[127]=0;
 std::lock_guard<std::mutex> lock(rootMutex);shaderBindings[list]=binding;
 if(binding.pso&&strstr(binding.name,"Character_Clothing")&&characterPipelines.size()<12&&!characterPipelines.count(binding.pso))characterPipelines[binding.pso]=reinterpret_cast<ID3D12PipelineState*>(binding.pso);
 if(strstr(binding.name,"Character_Clothing_GBuffer"))recordSceneShadow(4);
 UINT eye=presentCount.load();if(eye<2&&!driverParamsSeen[eye]&&strstr(binding.name,"Character_Clothing_GBuffer")){UINT64 layout=0;UINT count=0;if(read(address+0x520,&layout,8)&&read(layout+0x50,&count,4)&&count<=48){driverParamsSeen[eye]=true;for(UINT i=0;i<count;++i){DriverParameterRecord record{};record.eye=eye;record.slot=i;record.context=address;read(layout+0x60+i*8,&record.metadata,8);read(layout+0xb8+i*4,&record.root,4);if(!read(record.metadata,record.descriptor,sizeof(record.descriptor)))continue;memcpy(&record.id,record.descriptor+0xc08,4);memcpy(&record.count,record.descriptor+0xc90,4);if(record.id<47)read(address+0x540+record.id*8,&record.binding,8);read(record.binding,record.bound,sizeof(record.bound));driverParameters.push_back(record);}}}
}
static void worldUi(void* a,UINT b,void* c,void* d){
 Guard guard;if(captureEnabled.load()&&pairState.load()==1&&currentCameraMode.load()==8){++worldUiSuppressed;return;}
 originalWorldUi(a,b,c,d);
}
static void recordDraw(ID3D12GraphicsCommandList* list,UINT n,UINT instances,UINT first,INT vertex,UINT firstInstance,UINT indexed){
 if(!auditDraw())return;
 DrawRecord record{};record.list=reinterpret_cast<UINT64>(list);record.args[0]=n;record.args[1]=instances;record.args[2]=first;record.args[3]=static_cast<UINT>(vertex);record.args[4]=firstInstance;record.indexed=indexed;
 void* stack[4]{};USHORT depth=CaptureStackBackTrace(2,4,stack,nullptr);for(USHORT i=0;i<depth;++i)record.stack[i]=reinterpret_cast<UINT64>(stack[i]);
 std::lock_guard<std::mutex> lock(rootMutex);auto& binding=drawBindings[record.list];record.position=binding.position;memcpy(record.tiny,binding.tiny,8);auto& records=drawRecords[presentCount.load()];if(records.size()<32768){records.push_back(record);drawShaderRecords[presentCount.load()].push_back({record,shaderBindings[record.list]});}
 auto& shader=shaderBindings[record.list];if(strstr(shader.name,"Character")&&characterVertexRecords.size()<4096){CharacterVertexRecord item{};item.draw=record;memcpy(item.vertices,binding.vertices,sizeof(item.vertices));item.shader=shader.shader;item.eye=presentCount.load();memcpy(item.name,shader.name,128);characterVertexRecords.push_back(item);}
}
static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* list,UINT n,UINT instances,UINT first,UINT firstInstance){Guard guard;recordDraw(list,n,instances,first,0,firstInstance,0);originalDraw(list,n,instances,first,firstInstance);}
static void STDMETHODCALLTYPE drawIndexed(ID3D12GraphicsCommandList* list,UINT n,UINT instances,UINT first,INT vertex,UINT firstInstance){Guard guard;recordDraw(list,n,instances,first,vertex,firstInstance,1);originalDrawIndexed(list,n,instances,first,vertex,firstInstance);}
static void STDMETHODCALLTYPE vertexBinding(ID3D12GraphicsCommandList* list,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW* views){
 Guard guard;originalVertex(list,start,count,views);if(!auditDraw()||start>=8||!count||!views)return;
 D3D12_VERTEX_BUFFER_VIEW v[8]{};UINT n=std::min(count,8-start);if(read(reinterpret_cast<UINT64>(views),v,n*sizeof(v[0]))){std::lock_guard<std::mutex> lock(rootMutex);auto& binding=drawBindings[reinterpret_cast<UINT64>(list)];memcpy(binding.vertices+start,v,n*sizeof(v[0]));if(!start)binding.position=v[0].BufferLocation;}
}
static void STDMETHODCALLTYPE singleConstant(ID3D12GraphicsCommandList* list,UINT root,UINT value,UINT offset){
 Guard guard;originalSingleConstant(list,root,value,offset);if(!auditDraw()||root!=0||offset>=2)return;
 std::lock_guard<std::mutex> lock(rootMutex);drawBindings[reinterpret_cast<UINT64>(list)].tiny[offset]=value;
}
static void STDMETHODCALLTYPE constants(ID3D12GraphicsCommandList* list,UINT root,UINT count,const void* values,UINT offset){
 Guard guard;originalConstants(list,root,count,values,offset);
 if(auditDraw()&&root==0&&values&&offset<2){std::lock_guard<std::mutex> lock(rootMutex);read(reinterpret_cast<UINT64>(values),drawBindings[reinterpret_cast<UINT64>(list)].tiny+offset,std::min(count,2-offset)*4);}
 if(internal||!captureEnabled.load()||pairState.load()!=1||!auditThisPair.load())return;
 UINT eye=presentCount.load();if(eye>1)return;
 std::lock_guard<std::mutex> lock(rootMutex);auto& records=constantsRecords[eye];if(records.size()>=32768)return;
 ConstantsRecord record{};record.root=root;record.count=count;record.offset=offset;record.list=reinterpret_cast<UINT64>(list);
 if(values)read(reinterpret_cast<UINT64>(values),record.values,std::min(count,16u)*4);records.push_back(record);
}
static void captureBones(unsigned eye){
 for(UINT bank=0;bank<2;++bank)if(boneReaders[bank]){
  UINT states[2]{};if(!read(boneWrappers[bank]+0x60,states,8)||states[0]!=64||states[1]!=64||boneStates[bank]!=64||boneSplit[bank])throw std::runtime_error("Bone buffer state differs");
  internal=true;auto hr=boneReaders[bank]->capture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,boneBytes[eye][bank]);internal=false;check(hr);
 }
 if(driverReader){UINT states[2]{};if(!read(driverWrapper+0x60,states,8)||states[0]!=64||states[1]!=64||driverState!=64||driverSplit)throw std::runtime_error("Driver shader buffer state differs");internal=true;auto hr=driverReader->capture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,driverBytes[eye]);internal=false;check(hr);}
 if(uniformReader&&uniformState==D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER&&!uniformSplit){internal=true;auto hr=uniformReader->capture(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,uniformBytes[eye]);internal=false;check(hr);}

}
static void captureInstances(unsigned eye){
 if(!auditThisPair.load())return;
 
 UINT states[2]{};UINT64 ptr=0;
 if(!read(instanceWrapper+0x60,states,8)||!read(instanceWrapper+0xc8,&ptr,8)||ptr!=reinterpret_cast<UINT64>(instanceResource)||states[0]!=64||states[1]!=64||instanceState.load()!=64||instanceSplit.load())throw std::runtime_error("Instance resource state not confirmed");
 // The native append count is reset before this point. Capture a bounded,
 // readable prefix; offline analysis must restrict it to actual draw inputs.
 UINT64 cpuRecords=0;UINT count=0,capacity=0;if(read(base+0x12ca870,&cpuRecords,8)&&cpuRecords&&read(base+0x12d6104,&count,4)&&read(base+0x12ca878,&capacity,4)&&count<=capacity&&capacity<=98304){cpuInstanceRecords[eye].resize(static_cast<SIZE_T>(count)*64);if(!read(cpuRecords,cpuInstanceRecords[eye].data(),cpuInstanceRecords[eye].size()))cpuInstanceRecords[eye].clear();else{
 worldInputs[eye].clear();for(UINT i=0;i<count;++i){WorldInputRecord record{};record.index=i;memcpy(record.source,cpuInstanceRecords[eye].data()+static_cast<SIZE_T>(i)*64,64);memcpy(&record.metadata,record.source+52,4);UINT64 current=0,previous=0;memcpy(&current,record.source+8,8);memcpy(&previous,record.source+16,8);if(read(current,record.current,64)&&read(previous,record.previous,64))worldInputs[eye].push_back(record);}
 }}
 internal=true;auto hr=instanceReader->capture(static_cast<D3D12_RESOURCE_STATES>(instanceState.load()),instanceBytes[eye]);internal=false;check(hr);captureBones(eye);
}

static void deriveCamera(void* dst,const void* world,const void* projection,const void* previousWorld,const void* previousProjection,const void* policy,const void* origin){
 Guard guard;std::array<BYTE,16> copy{};
 if(preserveOffAxis.load(std::memory_order_acquire)&&reinterpret_cast<UINT64>(dst)==viewAddress+0x400&&copiedOffAxisPolicy(policy,copy)){++policyOverrides;originalDerived(dst,world,projection,previousWorld,previousProjection,copy.data(),origin);}
 else originalDerived(dst,world,projection,previousWorld,previousProjection,policy,origin);
}
static bool verifyGlass(unsigned eye){

 std::array<DirectX::XMFLOAT4X4,2> native{},shader{};
 glassClipAligned[eye]=read(viewAddress+0x500,native.data(),sizeof(native))&&read(shaderParamsAddress+0x100,shader.data(),sizeof(shader))&&matrixClose(native[0],native[1])&&matrixClose(shader[0],shader[1])&&matrixClose(native[0],shader[0]);return glassClipAligned[eye];
}
static void snapshot(void* ctx,unsigned i){recordSceneShadow(i+1);readOk[i]=read(reinterpret_cast<UINT64>(ctx),contexts[i].data(),40)&&read(cameraAddress,cameras[i].data(),64);if(base){readOk[i]=read(base+0x10da568,&counters[i],8)&&readOk[i];}}
static void resetObservation(){targetState=UINT_MAX;finalList=0;executedList=0;lastQueue=0;splitPending=false;}
static void STDMETHODCALLTYPE rootCbv(ID3D12GraphicsCommandList* list,UINT root,UINT64 address){
 Guard guard;originalRootCbv(list,root,address);
 if(internal||!captureEnabled.load()||pairState.load()!=1)return;
 UINT eye=presentCount.load();if(eye>1)return;
 std::lock_guard<std::mutex> lock(rootMutex);auto& records=rootRecords[eye];if(records.size()>=256)return;
 RootRecord record{};record.root=root;record.pool=UINT_MAX;record.address=address;record.list=reinterpret_cast<UINT64>(list);records.push_back(record);
}

static void uploadAddress(void* context){
 Guard guard;originalUploadAddress(context);
 if(internal||!captureEnabled.load()||pairState.load()!=1)return;
 std::lock_guard<std::mutex> lock(rootMutex);
 for(auto& record:uploadContexts)if(record.address==reinterpret_cast<UINT64>(context))return;
 if(uploadContexts.size()<16){ContextRecord record{};record.address=reinterpret_cast<UINT64>(context);read(record.address,record.bytes,sizeof(record.bytes));uploadContexts.push_back(record);}
 return;
}
static bool select(unsigned eye){
 ComPtr<ID3D12Resource> source;indices[eye]=gameSwap->GetCurrentBackBufferIndex();
 auto hr=gameSwap->GetBuffer(indices[eye],IID_PPV_ARGS(&source));
 if(FAILED(hr)){captureResults[eye]=hr;return false;}
 selectedSource=source;target=selectedSource.Get();resources[eye]=reinterpret_cast<UINT64>(target);observedTarget=target;resetObservation();return true;
}
static NativeFinalEye* finalEyes;
static MonoMonitor* monitorMirror=nullptr;static bool monoMonitor=false;
static std::array<UINT64,2> monoDraws{};static std::array<bool,2> monoRecorded{};
static UINT64 monoSkipped=0;
static void capture(unsigned eye){
 stateVerified[eye]=targetState.load()==D3D12_RESOURCE_STATE_PRESENT&&!splitPending.load()&&finalList.load()&&executedList.load()==finalList.load()&&lastQueue.load()==reinterpret_cast<UINT64>(gameQueue);
 if(!stateVerified[eye]){captureResults[eye]=E_UNEXPECTED;return;}
 internal=true;captureResults[eye]=currentCameraMode.load()==8&&finalEyes?finalEyes->capture(eye,gameQueue):store->capture(eye,target);if(eye==0&&hudCopied)hudCaptureResult=S_OK;internal=false;
}
static HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* swap,UINT interval,UINT flags){
 Guard guard;
 UINT observedEye=UINT_MAX;
 if(swap==gameSwap&&!(flags&DXGI_PRESENT_TEST)&&captureEnabled.load(std::memory_order_acquire)&&pairState.load(std::memory_order_acquire)==1){
  UINT eye=presentCount.fetch_add(1);observedEye=eye;
  if(eye<2&&resources[eye]&&gameSwap->GetCurrentBackBufferIndex()==indices[eye]){UINT64 tick=nowTicks();capture(eye);eyeCaptureTicks[eye].fetch_add(nowTicks()-tick);
   if(monoMonitor&&monitorMirror&&currentCameraMode.load()==8){
    if(SUCCEEDED(captureResults[eye])&&finalEyes->texture(0)){
     internal=true;auto mirrorHr=monitorMirror->submit(gameQueue,target,eye,hudArmed&&hudCopied,finalOutputLinear);internal=false;
     if(SUCCEEDED(mirrorHr)){monoRecorded[eye]=true;++monoDraws[eye];}else{captureResults[eye]=mirrorHr;stopRequested=true;++monoSkipped;}
    }else ++monoSkipped;
   }
  }
 }
 UINT64 tick=nowTicks();auto result=originalPresent(swap,interval,flags);
 if(observedEye<2&&currentCameraMode.load()==8){auto elapsed=nowTicks()-tick;presentTicks[observedEye].fetch_add(elapsed);auto previous=presentMaxTicks[observedEye].load();while(previous<elapsed&&!presentMaxTicks[observedEye].compare_exchange_weak(previous,elapsed)){};++presentCalls[observedEye];presentFlagMask[observedEye].fetch_or(flags);if(result!=S_OK)++presentNonOk[observedEye];if(interval<32)presentIntervalMask[observedEye].fetch_or(1u<<interval);}
 return result;
}
static bool apply(unsigned eye){preserveOffAxis=true;setView(reinterpret_cast<void*>(viewAddress),&eyeMatrices[eye]);dirt5_camera::NativeViewMatrices current{};applied[eye]=read(viewAddress+0x200,&current,sizeof(current))&&!memcmp(&current,&eyeMatrices[eye],sizeof(current));DirectX::XMFLOAT4X4 p{};fullOpticalCentre[eye]=read(viewAddress+0x480,&p,sizeof(p))&&matrixClose(p,eyeMatrices[eye].currentProjection);return applied[eye]&&fullOpticalCentre[eye];}
static void restore(){preserveOffAxis=false;setView(reinterpret_cast<void*>(viewAddress),&savedView);dirt5_camera::NativeViewMatrices current{};viewRestored=read(viewAddress+0x200,&current,sizeof(current))&&!memcmp(&current,&savedView,sizeof(current));}
static void cinema(void* ctx,bool a,bool b){
 captureEnabled=false;presentCount=0;resources={0,0};captureResults={E_PENDING,E_PENDING};bool called=false;
 try{
  if(!runtime->begin(false)){originalFrame(ctx,a,b);pairState=0;if(runtime->exiting())stopRequested=true;return;}
  if(!select(0))throw std::runtime_error("Cinema backbuffer unavailable");captureEnabled=true;
  UINT64 before=0,after=0;read(base+0x10da568,&before,8);called=true;originalFrame(ctx,a,b);read(base+0x10da568,&after,8);
  if(FAILED(captureResults[0])||presentCount.load()!=1||after!=before+1)throw std::runtime_error("Cinema native frame validation failed");
  internal=true;try{runtime->submitCinema(store->texture(0));}catch(...){internal=false;throw;}internal=false;++cinemaFrames;
 }catch(const std::exception& e){stopRequested=true;pairFailure=e.what();if(!called)originalFrame(ctx,a,b);}
 captureEnabled=false;pairState=0;
}
static bool serviceNativeResolution(void* ctx,bool a,bool b,bool race){
 bool desired=race&&nativeResizeEnabled&&!stopRequested.load();
 if(nativeResolutionOverride.load()!=desired){
  nativeResolutionOverride=desired;nativeResizePending=true;nativeWarmup=0;nativeWarmupUntil=0;
  if(desired){nativeResolutionWasUsed=true;nativeRestoreRequested=false;nativeRestoreDone=false;}
  else{nativeRestoreRequested=true;nativeRestoreDone=false;}
  // Clear borrowed pointers before the engine replaces its target pool.
  uiSurface=nullptr;uiSurfaceState=UINT_MAX;raceSceneCb=0;history={};
  originalFrame(ctx,a,b);requestNativeRebuild();return true;
 }
 if(nativeResizePending){
  UINT64 cfg=0;std::array<UINT,6> dims{};
  bool valid=read(base+0x12f2a18,&cfg,8)&&cfg&&read(cfg+0x3d0,dims.data(),sizeof(dims));
  bool ready=desired?(valid&&dims[0]==nativeEyeWidth&&dims[1]==nativeEyeHeight&&dims[2]==nativeEyeWidth&&dims[3]==nativeEyeHeight):nativeResolutionRestores.load()>0;
  originalFrame(ctx,a,b);
  if(ready&&!nativeWarmupUntil)nativeWarmupUntil=GetTickCount64()+(desired?1000:0);
  if(ready&&++nativeWarmup>=2&&GetTickCount64()>=nativeWarmupUntil){nativeResizePending=false;if(liveLog){if(desired)configureStereoSwapLatency(*liveLog);*liveLog<<"NATIVE_SCENE="<<dims[0]<<'x'<<dims[1]<<" history="<<dims[2]<<'x'<<dims[3]<<" desktop="<<dims[4]<<'x'<<dims[5]<<" mode="<<(desired?"VR":"DEFAULT")<<std::endl;logSwapState(*liveLog,desired?"SCENE_RESIZED":"SCENE_RESTORED");logGpuBudget(gameQueue,*liveLog,desired?"SCENE_RESIZED":"SCENE_RESTORED");}if(!desired)nativeRestoreDone=true;}
  return true;
 }
 return false;
}
// finalEyes is declared before Present-time capture.
using FinalOmFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
using FinalVpFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*);
using FinalScFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*);
static FinalOmFn finalOriginalOm=nullptr;static FinalVpFn finalOriginalVp=nullptr;static FinalScFn finalOriginalSc=nullptr;
static DrawFn finalOriginalDraw=nullptr;static FlushFn finalOriginalFlush=nullptr;
static thread_local UINT64 finalNativeList=0;static thread_local bool finalNativeShader=false;
static thread_local std::unordered_map<UINT64,bool> finalShaderCache;
struct FinalParameterRecord{UINT phase,width,height,reserved;DriverParameterRecord parameter;};
static std::vector<FinalParameterRecord> finalParameterRecords;static std::unordered_map<std::string,UINT> finalParameterCounts;static std::mutex finalParameterMutex;
using BatchWriteFn=void(*)(void*,const void*,UINT);static BatchWriteFn originalBatchWrite=nullptr;
struct BatchSnapshot{UINT bytes=0;UINT64 gpuAddress=0;std::array<BYTE,256> data{};};
static std::unordered_map<UINT64,BatchSnapshot> batchSnapshots;static std::mutex batchSnapshotMutex;
struct FinalBatchRecord{UINT phase,width,height,eye,bytes,reserved;UINT64 context,gpuAddress;BYTE data[256];};
static std::vector<FinalBatchRecord> finalBatchRecords;
static void batchWrite(void* wrapper,const void* data,UINT bytes){
 Guard guard;BatchSnapshot snapshot{};UINT64 context=0;
 bool valid=(parameterAudit||(independentFinalConstants&&bytes==128))&&bytes&&bytes<=snapshot.data.size()&&read(reinterpret_cast<UINT64>(wrapper)+0x40,&context,8)&&context&&read(reinterpret_cast<UINT64>(data),snapshot.data.data(),bytes);
 originalBatchWrite(wrapper,data,bytes);
 if(valid&&read(context+0x310,&snapshot.gpuAddress,8)){snapshot.bytes=bytes;std::lock_guard<std::mutex> lock(batchSnapshotMutex);if(batchSnapshots.size()<64||batchSnapshots.count(context))batchSnapshots[context]=snapshot;}
}
static void auditFinalParameters(UINT64 context){
 if(!parameterAudit)return;
 UINT64 cfg=0,layout=0;UINT dims[2]{},count=0;
 if(!read(base+0x12f2a18,&cfg,8)||!cfg||!read(cfg+0x3d0,dims,8)||!read(context+0x520,&layout,8)||!layout||!read(layout+0x50,&count,4)||count>48)return;
 UINT phase=nativeResolutionOverride.load()?(captureEnabled.load()?2:1):(nativeResolutionRestores.load()?3:0),eye=presentCount.load();
 auto key=std::to_string(phase)+":"+std::to_string(eye)+":"+std::to_string(dims[0])+"x"+std::to_string(dims[1]);
 std::lock_guard<std::mutex> lock(finalParameterMutex);if(finalParameterCounts[key]>=2)return;++finalParameterCounts[key];
 {std::lock_guard<std::mutex> batchLock(batchSnapshotMutex);auto found=batchSnapshots.find(context);UINT64 gpu=0;
  if(found!=batchSnapshots.end()&&read(context+0x310,&gpu,8)&&gpu==found->second.gpuAddress){FinalBatchRecord record{};record.phase=phase;record.width=dims[0];record.height=dims[1];record.eye=eye;record.bytes=found->second.bytes;record.context=context;record.gpuAddress=gpu;memcpy(record.data,found->second.data.data(),record.bytes);if(finalBatchRecords.size()<96)finalBatchRecords.push_back(record);}
 }
 for(UINT i=0;i<count&&finalParameterRecords.size()<384;++i){FinalParameterRecord record{};record.phase=phase;record.width=dims[0];record.height=dims[1];auto& p=record.parameter;p.context=context;p.slot=i;p.eye=eye;
  if(!read(layout+0x60+i*8,&p.metadata,8)||!read(layout+0xb8+i*4,&p.root,4)||!read(p.metadata,p.descriptor,sizeof(p.descriptor)))continue;
  memcpy(&p.id,p.descriptor+0xc08,4);memcpy(&p.count,p.descriptor+0xc90,4);if(p.id<47&&read(context+0x540+p.id*8,&p.binding,8))read(p.binding,p.bound,sizeof(p.bound));finalParameterRecords.push_back(record);
 }
}
static bool captureNativeFinal(){return finalEyes&&captureEnabled.load()&&pairState.load()==1&&currentCameraMode.load()==8;}
static void finalFlush(void* ctx){Guard guard;finalOriginalFlush(ctx);finalNativeShader=false;if(internal||(!captureNativeFinal()&&!parameterAudit))return;
 UINT64 shader=0;if(!read(reinterpret_cast<UINT64>(ctx)+0x30,&finalNativeList,8)||!read(reinterpret_cast<UINT64>(ctx)+0x530,&shader,8)||!shader)return;
 auto known=finalShaderCache.find(shader);if(known==finalShaderCache.end()){char name[128]{};if(!read(shader,name,127))return;known=finalShaderCache.emplace(shader,strcmp(name,"posteffects:HistoryToBackBuffer")==0).first;}finalNativeShader=known->second;if(finalNativeShader)auditFinalParameters(reinterpret_cast<UINT64>(ctx));
 if(finalNativeShader&&independentFinalConstants&&captureNativeFinal()){
  UINT64 context=reinterpret_cast<UINT64>(ctx),layout=0,gpu=0;UINT root=UINT_MAX;BYTE enabled=0;UINT eye=presentCount.load();
  if(eye<2&&read(context+0x520,&layout,8)&&layout&&read(layout+0x4c,&enabled,1)&&enabled&&read(layout+0xb4,&root,4)&&read(context+0x310,&gpu,8)){
   std::lock_guard<std::mutex> lock(batchSnapshotMutex);auto found=batchSnapshots.find(context);
   if(found!=batchSnapshots.end()&&found->second.gpuAddress==gpu&&finalEyes->setColourConstants(eye,root,gpu,found->second.data.data(),found->second.bytes))++independentConstantWrites;
  }
 }
}
static void STDMETHODCALLTYPE finalOm(ID3D12GraphicsCommandList* list,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,BOOL range,const D3D12_CPU_DESCRIPTOR_HANDLE* dsv){Guard guard;finalOriginalOm(list,count,rtvs,range,dsv);if(!internal&&captureNativeFinal())finalEyes->om(list,count,rtvs,range,dsv);}
static void STDMETHODCALLTYPE finalVp(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* values){Guard guard;finalOriginalVp(list,count,values);if(!internal&&captureNativeFinal())finalEyes->viewport(list,count,values);}
static void STDMETHODCALLTYPE finalSc(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* values){Guard guard;finalOriginalSc(list,count,values);if(!internal&&captureNativeFinal())finalEyes->scissor(list,count,values);}
static void STDMETHODCALLTYPE finalDraw(ID3D12GraphicsCommandList* list,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance){Guard guard;finalOriginalDraw(list,vertices,instances,firstVertex,firstInstance);
 if(internal||!captureNativeFinal()||!finalNativeShader||finalNativeList!=reinterpret_cast<UINT64>(list))return;
 UINT eye=presentCount.load();if(eye>1)return;internal=true;finalEyes->duplicate(eye,list,vertices,instances,firstVertex,firstInstance,finalOriginalOm,finalOriginalVp,finalOriginalSc,finalOriginalDraw);internal=false;
}
static void frame(void* ctx,bool a,bool b){
 Guard guard;
 bool race=activeCamera();
 if(a&&b&&pairState.load()==0){try{if(serviceNativeResolution(ctx,a,b,race))return;}catch(const std::exception& e){pairFailure=e.what();stopRequested=true;originalFrame(ctx,a,b);return;}}
 UINT expected=0;if(stopRequested.load()||!a||!b||!pairState.compare_exchange_strong(expected,1)){originalFrame(ctx,a,b);return;}
 UINT mode=race?1:0;
 if(lastMode.exchange(mode)!=mode){++modeSwitches;runtime->recenter();history={};if(liveLog)*liveLog<<"MODE="<<(race?"VR":"CINEMA")<<" cameraMode="<<currentCameraMode.load()<<std::endl;}
 if(requestedRecenter.exchange(false)){runtime->recenter();history={};}
 if(!race){cinema(ctx,a,b);return;}
 finalEyes->begin();monoRecorded={false,false};raceSceneCb=currentSceneCb();sceneAnimationBank=UINT_MAX;hudArmed=uiSurface.load()&&uiSurfaceState.load()!=UINT_MAX;hudCopied=false;hudClears=0;hudCaptureResult=E_PENDING;
 captureEnabled=false;presentCount=0;resources={0,0};captureResults={E_PENDING,E_PENDING};applied={false,false};retained={false,false};viewRestored=false;
 auditThisPair=false;
 {std::lock_guard<std::mutex> lock(poseMutex);poseCopies.clear();poseSeen.clear();rightValidatedHeaders.clear();{std::lock_guard<std::mutex> regionLock(poseRegionMutex);poseRegions.clear();}}
 bool saved=read(viewAddress+0x200,&savedView,sizeof(savedView)),called=false,modified=false;
 actorProbeStage=-1;
 if(actorCpuProbe&&saved){const auto& m=savedView.currentWorld;if(!actorProbeOriginValid){actorProbeOrigin={m._41,m._42,m._43};actorProbeOriginValid=true;}float x=m._41-actorProbeOrigin.x,y=m._42-actorProbeOrigin.y,z=m._43-actorProbeOrigin.z;auto pair=completedPairs.load();if(pair==0)actorProbeStage=0;else if(pair==16)actorProbeStage=1;else if(!actorProbeMoved&&x*x+y*y+z*z>=25){actorProbeStage=2;actorProbeMoved=true;}}
 float nearZ=savedView.currentProjection._43/(savedView.currentProjection._33+1),farZ=savedView.currentProjection._43/savedView.currentProjection._33;
 try{
  if(!saved||!std::isfinite(nearZ)||nearZ<=0||!std::isfinite(farZ)||farZ<=nearZ||std::abs(savedView.currentProjection._34+1)>.0001f)throw std::runtime_error("Native projection invalid");
  dirt5_camera::affineSeat(savedView.currentWorld);
  if(!runtime->begin()){originalFrame(ctx,a,b);pairState=0;if(runtime->exiting())stopRequested=true;return;}
  std::array<dirt5_camera::EyeMatrices,2> currentEyes{};for(unsigned eye=0;eye<2;++eye){auto m=dirt5_camera::build(savedView.currentWorld,runtime->headCentre,runtime->eyes[eye],1.f,nearZ,farZ);currentEyes[eye]=m;eyeMatrices[eye]=history.prepare(eye,m);}
  snapshot(ctx,0);if(!select(0))throw std::runtime_error("First backbuffer unavailable");captureEnabled=true;modified=true;if(!apply(0))throw std::runtime_error("First eye matrices rejected");
  called=true;UINT64 tick=nowTicks();originalFrame(ctx,a,b);leftTicks+=nowTicks()-tick;captureActorCpu(0);snapshot(ctx,1);dirt5_camera::NativeViewMatrices after{};retained[0]=read(viewAddress+0x200,&after,sizeof(after))&&!memcmp(&after,&eyeMatrices[0],sizeof(after));
  bool stable=readOk[0]&&readOk[1]&&contexts[0]==contexts[1]&&cameras[0]==cameras[1];
  if(!verifyGlass(0)||!stable||!retained[0]||FAILED(captureResults[0])||(base&&counters[1]!=counters[0]+1))throw std::runtime_error("First native eye validation failed");
  prepare();tick=nowTicks();patchPoses();patchTicks+=nowTicks()-tick;if(!select(1))throw std::runtime_error("Second backbuffer unavailable");if(!apply(1))throw std::runtime_error("Second eye matrices rejected");
  replay=true;tick=nowTicks();originalFrame(ctx,a,true);rightTicks+=nowTicks()-tick;captureActorCpu(1);tick=nowTicks();restorePoses();restoreTicks+=nowTicks()-tick;snapshot(ctx,2);retained[1]=read(viewAddress+0x200,&after,sizeof(after))&&!memcmp(&after,&eyeMatrices[1],sizeof(after));verifyGlass(1);restore();modified=false;
  if(!glassClipAligned[1]||!retained[1]||!viewRestored||FAILED(captureResults[1])||!readOk[2]||contexts[1]!=contexts[2]||cameras[1]!=cameras[2]||(base&&counters[2]!=counters[0]+2))throw std::runtime_error("Second native eye validation failed");
  tick=nowTicks();internal=true;try{if(hudArmed&&(!hudCopied||hudClears!=2||FAILED(hudCaptureResult)))throw std::runtime_error("HUD capture or stereo removal incomplete");runtime->submit({finalEyes->texture(0),finalEyes->texture(1)},hudArmed?hudCopy.Get():nullptr,finalOutputLinear);}catch(...){internal=false;throw;}internal=false;submitTicks+=nowTicks()-tick;history.commit(currentEyes);
  completedPairs.fetch_add(1,std::memory_order_release);
 }catch(const std::exception& e){stopRequested=true;pairFailure=e.what();restorePoses();if(modified)restore();if(!called)originalFrame(ctx,a,b);}
 captureEnabled=false;pairState.store(0,std::memory_order_release);
}
static void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* list,UINT n,const D3D12_RESOURCE_BARRIER* barriers){
 Guard guard;if(!internal&&pairState.load()==1)for(UINT i=0;i<n;++i)if(barriers[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&barriers[i].Transition.pResource==uiSurface.load())uiSurfaceState=barriers[i].Transition.StateAfter;
 if(!internal&&pairState.load(std::memory_order_acquire)==1)for(UINT i=0;i<n;++i){const auto& b=barriers[i];
  if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&b.Transition.pResource==observedTarget.load()){
   if(b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES){targetState=UINT_MAX;continue;}
   if(b.Flags==D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY){splitPending=true;continue;}
   if(b.Flags==D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)splitPending=false;
   targetState=b.Transition.StateAfter;finalList=reinterpret_cast<UINT64>(list);
  }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING&&(b.Aliasing.pResourceBefore==observedTarget.load()||b.Aliasing.pResourceAfter==observedTarget.load()))targetState=UINT_MAX;
 }
 originalBarrier(list,n,barriers);
}
static void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue,UINT n,ID3D12CommandList*const* lists){
 Guard guard;originalExecute(queue,n,lists);if(!internal&&finalEyes&&captureEnabled.load()&&pairState.load()==1)finalEyes->execute(queue,n,lists);
 if(!internal&&pairState.load(std::memory_order_acquire)==1)for(UINT i=0;i<n;++i)if(reinterpret_cast<UINT64>(lists[i])==finalList.load()){executedList=reinterpret_cast<UINT64>(lists[i]);lastQueue=reinterpret_cast<UINT64>(queue);}
}
static void writePng(const std::filesystem::path& path,const std::vector<BYTE>& rgba){
    auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);check(apartment);
    try{
        ComPtr<IWICImagingFactory> factory;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
        ComPtr<IWICStream> stream;check(factory->CreateStream(&stream));check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(&frame,nullptr));check(frame->Initialize(nullptr));check(frame->SetSize(width,height));
        auto pixel=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&pixel));if(pixel!=GUID_WICPixelFormat32bppBGRA)throw std::runtime_error("Unsupported WIC pixel format");
        auto bgra=rgba;for(size_t i=0;i<bgra.size();i+=4)std::swap(bgra[i],bgra[i+2]);
        check(frame->WritePixels(height,width*4,static_cast<UINT>(bgra.size()),bgra.data()));check(frame->Commit());check(encoder->Commit());
    }catch(...){CoUninitialize();throw;}
    CoUninitialize();
}

template<class T>static void releaseContainer(T& value){T empty;value.swap(empty);}
static void releasePrivateResources(std::ofstream& log){
 // Called only after all hook callbacks have left and XR has closed.
 privateDrainFence.Reset();if(privateDrainEvent){CloseHandle(privateDrainEvent);privateDrainEvent=nullptr;}
 delete runtime;runtime=nullptr;delete monitorMirror;monitorMirror=nullptr;delete finalEyes;finalEyes=nullptr;delete store;store=nullptr;
 delete hudReader;hudReader=nullptr;hudCopy.Reset();
 delete instanceReader;instanceReader=nullptr;instanceResource=nullptr;
 selectedSource.Reset();characterPipelines.clear();delete uniformReader;uniformReader=nullptr;uniformResource.Reset();uniformState=UINT_MAX;uniformSplit=false;
 delete driverReader;driverReader=nullptr;driverResource=nullptr;driverWrapper=0;driverState=64;driverSplit=false;for(auto& bytes:driverBytes)releaseContainer(bytes);
 for(UINT bank=0;bank<2;++bank){delete boneReaders[bank];boneReaders[bank]=nullptr;boneResources[bank]=nullptr;boneWrappers[bank]=0;boneStates[bank]=64;boneSplit[bank]=false;}
 for(auto& eye:boneBytes)for(auto& bytes:eye)releaseContainer(bytes);for(auto& bytes:uniformBytes)releaseContainer(bytes);
 for(auto& pool:uploadPools){if(pool.bytes){D3D12_RANGE none{0,0};pool.resource->Unmap(0,&none);}pool={};}
 if(swapOwned&&gameSwap)gameSwap->Release();if(queueOwned&&gameQueue)gameQueue->Release();
 swapOwned=queueOwned=false;gameSwap=nullptr;gameQueue=nullptr;target=nullptr;observedTarget=nullptr;uiSurface=nullptr;
 if(stopEvent){CloseHandle(stopEvent);stopEvent=nullptr;}if(recenterEvent){CloseHandle(recenterEvent);recenterEvent=nullptr;}
 for(auto& records:rootRecords)releaseContainer(records);
 for(auto& records:drawRecords)releaseContainer(records);
 for(auto& records:drawShaderRecords)releaseContainer(records);
 for(auto& records:worldInputs)releaseContainer(records);
 for(auto& records:instanceBytes)releaseContainer(records);
 for(auto& records:cpuInstanceRecords)releaseContainer(records);
 for(auto& records:constantsRecords)releaseContainer(records);
 releaseContainer(poseCopies);releaseContainer(poseSeen);releaseContainer(poseRegions);
 releaseContainer(completeBanks);releaseContainer(bankMisses);releaseContainer(firstBanks);
 releaseContainer(drawBindings);releaseContainer(shaderBindings);releaseContainer(currentPso);
 releaseContainer(dispatchRecords);releaseContainer(skinRenderRecords);releaseContainer(characterVertexRecords);
 releaseContainer(sceneConstantRecords);releaseContainer(driverParameters);releaseContainer(uploadContexts);releaseContainer(uiRecords);
 sceneShadowAddress=discoveredSceneBinding=discoveredSceneCb=0;sceneConstantResource=nullptr;sceneMapped=nullptr;
 log<<"RESOURCES_RELEASED: eye/HUD textures, readbacks, pipeline references, trace buffers and event handles"<<std::endl;
 liveLog=nullptr;
}
static DWORD run(){
 wchar_t path[32768]{};GetModuleFileNameW(self,path,32768);auto folder=std::filesystem::path(path).parent_path();auto traces=folder/L"traces";std::filesystem::create_directories(traces);auto stem=L"native-xr-full-output-"+std::to_wstring(GetCurrentProcessId());std::ofstream log(traces/(stem+L".log"));liveLog=&log;
 try{
  base=reinterpret_cast<UINT64>(GetModuleHandleW(L"DIRT5.exe"));void* entry=nullptr;
  if(base){
   const BYTE expected[]={0x48,0x8b,0xc4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x18,0xfe,0xff,0xff};BYTE bytes[sizeof(expected)]{};
   if(!read(base+0x34b1c0,bytes,sizeof(bytes))||memcmp(bytes,expected,sizeof(bytes)))throw std::runtime_error("Native frame fingerprint differs");
   const BYTE prepExpected[]={0x48,0x83,0xec,0x28,0x48,0x8d,0x4c,0x24,0x38};BYTE p[sizeof(prepExpected)]{};
   if(!read(base+0x109970,p,sizeof(p))||memcmp(p,prepExpected,sizeof(p)))throw std::runtime_error("Native reuse prepare differs");
   UINT64 wrapper=0,queueWrapper=0,swapAddress=0,queueAddress=0;
   auto graphicsDeadline=GetTickCount64()+60000;
   while(GetTickCount64()<graphicsDeadline){
    if(read(base+0x10e7330,&wrapper,8)&&wrapper&&read(wrapper+0x78,&swapAddress,8)&&swapAddress&&read(base+0x10da558,&queueWrapper,8)&&queueWrapper&&read(queueWrapper,&queueAddress,8)&&queueAddress)break;Sleep(100);
   }
   if(!swapAddress||!queueAddress)throw std::runtime_error("Game graphics not ready");
   gameSwap=reinterpret_cast<IDXGISwapChain3*>(swapAddress);gameQueue=reinterpret_cast<ID3D12CommandQueue*>(queueAddress);
   const BYTE derivedExpected[]={0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54};BYTE derivedActual[sizeof(derivedExpected)]{};
   if(!read(base+0x192a90,derivedActual,sizeof(derivedActual))||memcmp(derivedActual,derivedExpected,sizeof(derivedExpected)))throw std::runtime_error("Derived camera fingerprint differs");
   const BYTE setterBytes[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x81,0xc1,0,2,0,0};BYTE setterActual[16]{};
   if(!read(base+0x1b1760,setterActual,16)||memcmp(setterBytes,setterActual,16))throw std::runtime_error("View setter fingerprint differs");
   entry=reinterpret_cast<void*>(base+0x34b1c0);prepare=reinterpret_cast<PrepareFn>(base+0x109970);
  }else throw std::runtime_error("Native DIRT5 host required");
  if(!entry||!prepare||!gameSwap||!gameQueue)throw std::runtime_error("Native graphics missing");
  ComPtr<ID3D12Device> dummyDevice;ComPtr<ID3D12CommandQueue> dummyQueue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> dummy;
  check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dummyDevice)));D3D12_COMMAND_QUEUE_DESC q{};check(dummyDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&dummyQueue)));check(dummyDevice->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));check(dummyDevice->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=16;td.Height=16;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
  check(dummyDevice->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&dummy)));
  // Validate public swapchain methods against a fresh platform object before calls.
  WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=self;wc.lpszClassName=L"DIRT5ModValidation";
  if(!RegisterClassW(&wc))throw std::runtime_error("Window class failed");
  auto window=CreateWindowW(wc.lpszClassName,L"Validation",WS_POPUP,0,0,16,16,nullptr,nullptr,self,nullptr);
  if(!window)throw std::runtime_error("Window failed");
  ComPtr<IDXGIFactory4> factory;ComPtr<IDXGISwapChain1> swap1;ComPtr<IDXGISwapChain3> swap3;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=16;sd.Height=16;
  sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  check(factory->CreateSwapChainForHwnd(dummyQueue.Get(),window,&sd,nullptr,nullptr,&swap1));check(swap1.As(&swap3));
  UINT64 swapTable=0;std::array<void*,37> ss{};
  if(!read(reinterpret_cast<UINT64>(gameSwap),&swapTable,8)||!read(swapTable,ss.data(),sizeof(ss)))throw std::runtime_error("Swap unreadable");
  auto ds=*reinterpret_cast<void***>(swap3.Get());
  for(auto slot:{0,1,2,7,9,12,36})if(ss[slot]!=ds[slot])throw std::runtime_error("Swap public API fingerprint differs");
  ComPtr<ID3D12Device> swapDevice;check(gameSwap->GetDevice(IID_PPV_ARGS(&swapDevice)));
  ComPtr<ID3D12Resource> initial;check(gameSwap->GetBuffer(gameSwap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&initial)));target=initial.Get();
  gameSwap->AddRef();swapOwned=true;swap3.Reset();swap1.Reset();DestroyWindow(window);UnregisterClassW(wc.lpszClassName,self);
  UINT64 resourceTable=0,queueTable=0;std::array<void*,15> rs{};std::array<void*,19> qs{};
  if(!read(reinterpret_cast<UINT64>(target),&resourceTable,8)||!read(resourceTable,rs.data(),sizeof(rs))||!read(reinterpret_cast<UINT64>(gameQueue),&queueTable,8)||!read(queueTable,qs.data(),sizeof(qs)))throw std::runtime_error("Observed resource/queue unreadable");
  auto dr=*reinterpret_cast<void***>(dummy.Get()),dq=*reinterpret_cast<void***>(dummyQueue.Get());

  for(UINT slot=0;slot<15;++slot)resourceMethods[slot]=rs[slot];
  for(auto s:{0,1,2,7,10})if(rs[s]!=dr[s])throw std::runtime_error("Resource API fingerprint differs");for(auto s:{0,1,2,7,10,14,18})if(qs[s]!=dq[s])throw std::runtime_error("Queue API fingerprint differs");
  ComPtr<ID3D12Device> device,sourceDevice;check(gameQueue->GetDevice(IID_PPV_ARGS(&device)));check(target->GetDevice(IID_PPV_ARGS(&sourceDevice)));ComPtr<IUnknown> a,b;device.As(&a);sourceDevice.As(&b);if(a.Get()!=b.Get())throw std::runtime_error("Resource/queue device mismatch");
  ComPtr<IUnknown> c;swapDevice.As(&c);if(a.Get()!=c.Get())throw std::runtime_error("Swap/queue device mismatch");
  auto desc=target->GetDesc();if(desc.Width>8192)throw std::runtime_error("Target too large");width=static_cast<UINT>(desc.Width);height=desc.Height;format=desc.Format;
  gameQueue->AddRef();queueOwned=true;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&privateDrainFence)));privateDrainSerial=0;privateDrainEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!privateDrainEvent)throw std::runtime_error("Private GPU drain event failed");store=new EyeGpuStore;check(store->setup(device.Get(),gameQueue,width,height,format,false));
  D3D12_RESOURCE_DESC hudDesc{};hudDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;hudDesc.Width=width;hudDesc.Height=height;hudDesc.DepthOrArraySize=1;hudDesc.MipLevels=1;hudDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;hudDesc.SampleDesc.Count=1;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&hudDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&hudCopy)));
  logGpuBudget(gameQueue,log,"BEFORE_XR");
  logSwapState(log,"BEFORE_XR");
  runtime=new dirt5_xr_pair::Runtime;runtime->setup(folder,gameQueue,log);nativeEyeWidth=runtime->width;nativeEyeHeight=runtime->height;
  nativeResizeEnabled=GetPrivateProfileIntW(L"Mod",L"NativeSceneResize",1,(folder/L"settings.ini").c_str())!=0;
  finalOutputLinear=GetPrivateProfileIntW(L"Mod",L"FinalOutputLinear",1,(folder/L"settings.ini").c_str())!=0;
  directFinalOutput=GetPrivateProfileIntW(L"Mod",L"DirectFinalOutput",1,(folder/L"settings.ini").c_str())!=0;
  independentFinalConstants=GetPrivateProfileIntW(L"Mod",L"IndependentFinalConstants",1,(folder/L"settings.ini").c_str())!=0;independentConstantWrites=0;
  rightMatrixBankSelection=GetPrivateProfileIntW(L"Mod",L"RightMatrixBankSelection",1,(folder/L"settings.ini").c_str())!=0;rightDescriptorSelections=rightRecordSelections=0;
  actorCpuProbe=GetPrivateProfileIntW(L"Mod",L"ActorCpuProbe",0,(folder/L"settings.ini").c_str())!=0;actorProbeStage=-1;actorProbeOriginValid=actorProbeMoved=false;for(auto& snapshot:actorCpuSnapshots){snapshot.header={};snapshot.records.clear();}
  stereoSwapLatency=GetPrivateProfileIntW(L"Mod",L"StereoSwapLatency",1,(folder/L"settings.ini").c_str())!=0;
  configureStereoSwapLatency(log);
  parameterAudit=GetPrivateProfileIntW(L"Mod",L"ParameterAudit",0,(folder/L"settings.ini").c_str())!=0;
  resolutionUniformRecords.clear();resolutionUniformCounts.clear();
  finalParameterRecords.clear();finalParameterCounts.clear();
  finalBatchRecords.clear();batchSnapshots.clear();
  finalEyes=new NativeFinalEye;check(finalEyes->setup(device.Get(),gameQueue,runtime->width,runtime->height,width,height,directFinalOutput,independentFinalConstants));
  monoMonitor=GetPrivateProfileIntW(L"Mod",L"MonoMonitor",1,(folder/L"settings.ini").c_str())!=0;monoDraws={0,0};monoSkipped=0;
  if(monoMonitor){monitorMirror=new MonoMonitor;check(monitorMirror->setup(device.Get(),finalEyes->privateTarget(0),hudCopy.Get(),format));}
  log<<"FINAL_EYE_TARGETS="<<runtime->width<<'x'<<runtime->height<<" NativeSceneResize="<<nativeResizeEnabled<<" FinalOutputLinear="<<finalOutputLinear<<" DirectFinalOutput="<<directFinalOutput<<std::endl;
  logGpuBudget(gameQueue,log,"XR_OUTPUT_READY");
  if(MH_Initialize()!=MH_OK)throw std::runtime_error("Hook init failed");hooksInitialized=true;
  auto add=[&](void* addr,void* callback,void** original){if(MH_CreateHook(addr,callback,original)!=MH_OK||MH_QueueEnableHook(addr)!=MH_OK)throw std::runtime_error("Hook setup failed");};
  const BYTE finalFlushExpected[]={0x40,0x53,0x48,0x83,0xec,0x60,0x48,0x8b,0xd9};BYTE finalFlushActual[sizeof(finalFlushExpected)]{};
  if(!read(base+0x10c860,finalFlushActual,sizeof(finalFlushActual))||memcmp(finalFlushActual,finalFlushExpected,sizeof(finalFlushActual)))throw std::runtime_error("Final composition flush fingerprint differs");
  add(reinterpret_cast<void*>(base+0x10c860),reinterpret_cast<void*>(finalFlush),reinterpret_cast<void**>(&finalOriginalFlush));
  if(parameterAudit||independentFinalConstants){const BYTE expected[]={0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x79,0x40};BYTE actual[sizeof(expected)]{};if(!read(base+0x10a190,actual,sizeof(actual))||memcmp(actual,expected,sizeof(actual)))throw std::runtime_error("Batch constant writer fingerprint differs");add(reinterpret_cast<void*>(base+0x10a190),reinterpret_cast<void*>(batchWrite),reinterpret_cast<void**>(&originalBatchWrite));}
  auto finalMethods=*reinterpret_cast<void***>(list.Get());
  add(finalMethods[12],reinterpret_cast<void*>(finalDraw),reinterpret_cast<void**>(&finalOriginalDraw));
  add(finalMethods[46],reinterpret_cast<void*>(finalOm),reinterpret_cast<void**>(&finalOriginalOm));
  add(finalMethods[21],reinterpret_cast<void*>(finalVp),reinterpret_cast<void**>(&finalOriginalVp));
  add(finalMethods[22],reinterpret_cast<void*>(finalSc),reinterpret_cast<void**>(&finalOriginalSc));
  const BYTE nativeDimsExpected[]={0x48,0x8b,0xc4,0x48,0x89,0x58,8,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20};BYTE nativeDimsActual[sizeof(nativeDimsExpected)]{};
  const BYTE nativeRefreshExpected[]={0x38,0x05,0x0f,0x3d,0xf9,0,0x74,0x17};BYTE nativeRefreshActual[sizeof(nativeRefreshExpected)]{};
  if(!read(base+0x35a0e0,nativeDimsActual,sizeof(nativeDimsActual))||memcmp(nativeDimsActual,nativeDimsExpected,sizeof(nativeDimsActual))||!read(base+0x35ecf8,nativeRefreshActual,sizeof(nativeRefreshActual))||memcmp(nativeRefreshActual,nativeRefreshExpected,sizeof(nativeRefreshActual)))throw std::runtime_error("Native resolution lifecycle fingerprint differs");
  add(reinterpret_cast<void*>(base+0x35a0e0),reinterpret_cast<void*>(nativeDimensions),reinterpret_cast<void**>(&originalNativeDimensions));
  const BYTE worldUiExpected[]={0x41,0x8b,0x41,0x34,0x48,0x8d,0x14,0x40,0x48,0x8d,0x05,0x01,0x92,0,1};BYTE worldUiActual[sizeof(worldUiExpected)]{};
  if(!read(base+0x3f6e00,worldUiActual,sizeof(worldUiActual))||memcmp(worldUiActual,worldUiExpected,sizeof(worldUiExpected)))throw std::runtime_error("World UI callback fingerprint differs");
  const BYTE uniformExpected[]={0x4c,0x8b,0x1d,0xb9,0x95,0xfe,0};BYTE uniformActual[sizeof(uniformExpected)]{};
  if(!read(base+0x100780,uniformActual,sizeof(uniformActual))||memcmp(uniformActual,uniformExpected,sizeof(uniformExpected)))throw std::runtime_error("Scene uniform writer fingerprint differs");
  add(reinterpret_cast<void*>(base+0x100780),reinterpret_cast<void*>(writeUniform),reinterpret_cast<void**>(&originalUniformWrite));
  add(reinterpret_cast<void*>(base+0x3f6e00),reinterpret_cast<void*>(worldUi),reinterpret_cast<void**>(&originalWorldUi));
  const BYTE matrixExpected[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x70,0x20,0x57,0x48,0x81,0xec,0x10,1,0,0};BYTE matrixActual[sizeof(matrixExpected)]{};
  if(!read(base+0x1da660,matrixActual,sizeof(matrixActual))||memcmp(matrixActual,matrixExpected,sizeof(matrixActual)))throw std::runtime_error("Matrix writer fingerprint differs");
  const BYTE bulkExpected[]={0x4c,0x8b,0xdc,0x55,0x56,0x41,0x54,0x41,0x55,0x41,0x57};BYTE bulkActual[sizeof(bulkExpected)]{};
  if(!read(base+0x184150,bulkActual,sizeof(bulkActual))||memcmp(bulkActual,bulkExpected,sizeof(bulkActual)))throw std::runtime_error("Bulk matrix writer fingerprint differs");
  const BYTE descExpected[]={0x40,0x56,0x57,0x41,0x54,0x41,0x56,0x48,0x83,0xec,0x48};BYTE descActual[sizeof(descExpected)]{};
  if(!read(base+0x1c9f00,descActual,sizeof(descActual))||memcmp(descActual,descExpected,sizeof(descActual)))throw std::runtime_error("World descriptor fingerprint differs");
  if(!read(base+0x1da450,matrixActual,sizeof(matrixActual))||memcmp(matrixActual,matrixExpected,sizeof(matrixActual)))throw std::runtime_error("Descriptor matrix writer fingerprint differs");
  const BYTE uiExpected[]={0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20};BYTE uiActual[sizeof(uiExpected)]{};
  if(!read(base+0x3d30d0,uiActual,sizeof(uiActual))||memcmp(uiActual,uiExpected,sizeof(uiActual)))throw std::runtime_error("UI renderer fingerprint differs");
  const BYTE targetsExpected[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x60};BYTE targetsActual[sizeof(targetsExpected)]{};
  if(!read(base+0x10a700,targetsActual,sizeof(targetsActual))||memcmp(targetsActual,targetsExpected,sizeof(targetsActual)))throw std::runtime_error("UI target setter fingerprint differs");
  add(reinterpret_cast<void*>(base+0x3d30d0),reinterpret_cast<void*>(uiPass),reinterpret_cast<void**>(&originalUiPass));
  add(reinterpret_cast<void*>(base+0x10a700),reinterpret_cast<void*>(uiTargets),reinterpret_cast<void**>(&originalUiTargets));
  add(reinterpret_cast<void*>(base+0x1c9f00),reinterpret_cast<void*>(worldDesc),reinterpret_cast<void**>(&originalWorldDesc));
  add(reinterpret_cast<void*>(base+0x1da450),reinterpret_cast<void*>(matrixB),reinterpret_cast<void**>(&originalMatrixB));
  add(reinterpret_cast<void*>(base+0x1da660),reinterpret_cast<void*>(matrixA),reinterpret_cast<void**>(&originalMatrixA));
  add(reinterpret_cast<void*>(base+0x192a90),reinterpret_cast<void*>(deriveCamera),reinterpret_cast<void**>(&originalDerived));
  add(entry,reinterpret_cast<void*>(frame),reinterpret_cast<void**>(&originalFrame));add((*reinterpret_cast<void***>(list.Get()))[26],reinterpret_cast<void*>(barrier),reinterpret_cast<void**>(&originalBarrier));add(dq[10],reinterpret_cast<void*>(execute),reinterpret_cast<void**>(&originalExecute));
  add(ds[8],reinterpret_cast<void*>(present),reinterpret_cast<void**>(&originalPresent));
  if(MH_ApplyQueued()!=MH_OK)throw std::runtime_error("Hook enable failed");log<<"READY: native OpenXR animation fix; GPU readbacks disabled; menu quad / cockpit stereo; independent scene resize="<<nativeResizeEnabled<<"; final native colour composition targets follow headset dimensions; both native Present calls preserved"<<std::endl;state=1;
  stopEvent=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\DIRT5OpenXR.Stop."+std::to_wstring(GetCurrentProcessId())).c_str());
  recenterEvent=CreateEventW(nullptr,FALSE,FALSE,(L"Local\\DIRT5OpenXR.Recenter."+std::to_wstring(GetCurrentProcessId())).c_str());
  UINT testSeconds=GetPrivateProfileIntW(L"Mod",L"DurationSeconds",0,(folder/L"settings.ini").c_str());
  auto liveBegin=GetTickCount64();auto deadline=testSeconds?liveBegin+static_cast<UINT64>(testSeconds)*1000:UINT64_MAX;
  bool ctrlWasDown=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;
  while(!stopRequested.load()&&GetTickCount64()<deadline){
   if(stopEvent&&WaitForSingleObject(stopEvent,10)==WAIT_OBJECT_0)break;
   if(!stopEvent)Sleep(10);
   if(recenterEvent&&WaitForSingleObject(recenterEvent,0)==WAIT_OBJECT_0)requestedRecenter=true;
   bool ctrlDown=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;
   if(ctrlDown&&!ctrlWasDown){DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);if(foregroundPid==GetCurrentProcessId())requestedRecenter=true;}
   ctrlWasDown=ctrlDown;
  }
  stopRequested=true;
  // Finish a pair before removing its Present/barrier/queue observations.
  auto pairDeadline=GetTickCount64()+6000;while(pairState.load(std::memory_order_acquire)==1&&GetTickCount64()<pairDeadline)Sleep(1);
  auto restoreDeadline=GetTickCount64()+6000;
  while(nativeResolutionWasUsed&&(nativeResolutionOverride.load()||!nativeRestoreDone.load())&&GetTickCount64()<restoreDeadline)Sleep(10);
  if(nativeResolutionWasUsed&&(nativeResolutionOverride.load()||!nativeRestoreDone.load()))throw std::runtime_error("Native scene resolution restoration incomplete");
  auto liveElapsed=GetTickCount64()-liveBegin;
  log<<"NativeResolutionChanges="<<nativeResolutionChanges.load()<<" restores="<<nativeResolutionRestores.load()<<" requested="<<nativeEyeWidth<<'x'<<nativeEyeHeight<<" desktop="<<width<<'x'<<height<<std::endl;
  LARGE_INTEGER timingFrequency{};QueryPerformanceFrequency(&timingFrequency);double timingMs=1000.0/static_cast<double>(timingFrequency.QuadPart);
  log<<"PAIR_TIMING_MS left="<<leftTicks*timingMs<<" right="<<rightTicks*timingMs<<" patch="<<patchTicks*timingMs<<" restore="<<restoreTicks*timingMs<<" submit="<<submitTicks*timingMs<<" pairs="<<completedPairs.load()<<std::endl;
  for(UINT eye=0;eye<2;++eye)log<<"PRESENT_TIMING eye="<<eye<<" calls="<<presentCalls[eye].load()<<" intervalMask="<<presentIntervalMask[eye].load()<<" presentMs="<<presentTicks[eye].load()*timingMs<<" captureMs="<<eyeCaptureTicks[eye].load()*timingMs<<" maxPresentMs="<<presentMaxTicks[eye].load()*timingMs<<" flags="<<presentFlagMask[eye].load()<<" nonOk="<<presentNonOk[eye].load()<<std::endl;
  bool disarmed=MH_DisableHook(MH_ALL_HOOKS)==MH_OK;log<<(disarmed?"DISARMED: all hook entries restored; waiting for callbacks before resource release":"DISARM_FAILED")<<std::endl;
  auto settle=GetTickCount64()+2000;while(active.load(std::memory_order_acquire)&&GetTickCount64()<settle)Sleep(1);
  if(!disarmed||active.load()||pairState.load()!=0||(!completedPairs.load()&&!cinemaFrames.load()))throw std::runtime_error("Pair incomplete or callbacks active");
  log<<"MONO_MONITOR enabled="<<monoMonitor<<" left="<<monoDraws[0]<<" right="<<monoDraws[1]<<" skipped="<<monoSkipped<<" bothPresentCallsPreserved=1"<<std::endl;
  log<<"NativeFinalDraws="<<finalEyes->totalDraws.load()<<" finalCapture="<<nativeEyeWidth<<'x'<<nativeEyeHeight<<std::endl;
  log<<"INDEPENDENT_FINAL_CONSTANTS enabled="<<independentFinalConstants<<" writes="<<independentConstantWrites<<std::endl;
  log<<"RIGHT_MATRIX_BANK_SELECTION enabled="<<rightMatrixBankSelection<<" descriptors="<<rightDescriptorSelections.load()<<" records="<<rightRecordSelections.load()<<std::endl;
  if(actorCpuProbe)for(auto& snapshot:actorCpuSnapshots){if(snapshot.records.empty())continue;std::ofstream actorOut(traces/(stem+L"-actors-stage"+std::to_wstring(snapshot.header.stage)+L"-eye"+std::to_wstring(snapshot.header.eye)+L".bin"),std::ios::binary);actorOut.write(reinterpret_cast<const char*>(&snapshot.header),sizeof(snapshot.header));actorOut.write(reinterpret_cast<const char*>(snapshot.records.data()),snapshot.records.size()*sizeof(ActorCpuRecord));log<<"ACTOR_CPU_SNAPSHOT stage="<<snapshot.header.stage<<" eye="<<snapshot.header.eye<<" pair="<<snapshot.header.pair<<" records="<<snapshot.records.size()<<" stride="<<sizeof(ActorCpuRecord)<<std::endl;}
  if(parameterAudit){std::ofstream uniformOut(traces/(stem+L"-resolution-uniforms.bin"),std::ios::binary);uniformOut.write(reinterpret_cast<const char*>(resolutionUniformRecords.data()),resolutionUniformRecords.size()*sizeof(ResolutionUniformRecord));log<<"RESOLUTION_UNIFORMS records="<<resolutionUniformRecords.size()<<" stride="<<sizeof(ResolutionUniformRecord)<<std::endl;}
  if(parameterAudit){std::ofstream paramOut(traces/(stem+L"-final-parameters.bin"),std::ios::binary);paramOut.write(reinterpret_cast<const char*>(finalParameterRecords.data()),finalParameterRecords.size()*sizeof(FinalParameterRecord));log<<"FINAL_PARAMETERS records="<<finalParameterRecords.size()<<" stride="<<sizeof(FinalParameterRecord)<<std::endl;}
  if(parameterAudit){std::ofstream batchOut(traces/(stem+L"-final-batches.bin"),std::ios::binary);batchOut.write(reinterpret_cast<const char*>(finalBatchRecords.data()),finalBatchRecords.size()*sizeof(FinalBatchRecord));log<<"FINAL_BATCHES records="<<finalBatchRecords.size()<<" stride="<<sizeof(FinalBatchRecord)<<std::endl;}
  if(!drainPrivateQueue(log))throw std::runtime_error("Private GPU work still pending");
  restoreSwapLatency(log);runtime->close(log);
  log<<"AnimationBankOverrides="<<animationBankOverrides.load()<<" stereoPairs="<<completedPairs.load()<<" GPU_READBACKS=OFF"<<std::endl;
releasePrivateResources(log);
  if(MH_Uninitialize()!=MH_OK)throw std::runtime_error("Hook cleanup failed");hooksInitialized=false;
  std::ofstream out(traces/(stem+L".json"));out<<"{\"pid\":"<<GetCurrentProcessId()<<",\"stereoPairs\":"<<completedPairs.load()<<",\"cinemaFrames\":"<<cinemaFrames.load()<<",\"modeSwitches\":"<<modeSwitches.load()<<",\"cameraMode\":"<<currentCameraMode.load()<<",\"elapsedMs\":"<<liveElapsed<<",\"hooksRestored\":true,\"xrClosed\":true,\"failure\":"<<std::quoted(pairFailure)<<",\"independentSceneResolution\":"<<((nativeResolutionWasUsed.load()&&nativeResolutionChanges.load()&&completedPairs.load())?"true":"false")<<",\"eyeWidth\":"<<nativeEyeWidth<<",\"eyeHeight\":"<<nativeEyeHeight<<",\"monoMonitor\":"<<(monoMonitor?"true":"false")<<"}\n";
  if(!pairFailure.empty()||(!completedPairs.load()&&!cinemaFrames.load()))throw std::runtime_error("No valid mod frames or callback failure");
  log<<"NATIVE_XR_MOD_STOPPED: hooks restored; XR closed; stereoPairs="<<completedPairs.load()<<" cinemaFrames="<<cinemaFrames.load()<<std::endl;state=3;workerBusy=false;return 0;
 }catch(const std::exception& e){
  restoreSwapLatency(log);
  stopRequested=true;bool disarmed=!hooksInitialized||MH_DisableHook(MH_ALL_HOOKS)==MH_OK;
  auto settle=GetTickCount64()+4000;while(active.load()&&GetTickCount64()<settle)Sleep(1);
  bool closed=runtime==nullptr;
  if(disarmed&&!active.load()&&runtime){try{runtime->close(log);closed=true;}catch(const std::exception& closeError){runtime->restoreEnvironment();log<<"XR_CLOSE_DEFERRED: "<<closeError.what()<<std::endl;}}
  if(disarmed&&!active.load()&&pairState.load()==0&&closed&&drainPrivateQueue(log)){releasePrivateResources(log);if(hooksInitialized&&MH_Uninitialize()==MH_OK)hooksInitialized=false;}
  else{if(runtime)runtime->restoreEnvironment();liveLog=nullptr;log<<"RESOURCE_RELEASE_DEFERRED: callbacks or XR transfer still active"<<std::endl;}
  log<<"FAIL: "<<e.what()<<std::endl;state=2;return 1;
 }
}
static DWORD WINAPI init(void*){try{auto result=run();workerBusy=false;return result;}catch(...){MH_DisableHook(MH_ALL_HOOKS);state=2;workerBusy=false;return 1;}}
extern "C" __declspec(dllexport) DWORD WINAPI DIRT5ModReady(void*){return state.load();}
extern "C" __declspec(dllexport) DWORD WINAPI DIRT5ModRestart(void*){
 bool expected=false;if(!workerBusy.compare_exchange_strong(expected,true))return 1;
 if(state.load()!=3){workerBusy=false;return 2;}
 nativeResolutionOverride=false;nativeRestoreRequested=false;nativeRestoreDone=true;nativeResolutionChanges=0;nativeResolutionRestores=0;nativeResizePending=false;nativeWarmup=0;nativeResolutionWasUsed=false;
 if(swapLatencyOwned){workerBusy=false;return 3;}
 for(UINT eye=0;eye<2;++eye){presentTicks[eye]=0;eyeCaptureTicks[eye]=0;presentCalls[eye]=0;presentIntervalMask[eye]=0;presentMaxTicks[eye]=0;presentFlagMask[eye]=0;presentNonOk[eye]=0;}
 stopRequested=false;captureEnabled=false;completedPairs=0;cinemaFrames=0;modeSwitches=0;lastMode=UINT_MAX;pairState=0;active=0;previousCamera=0;policyOverrides=0;history={};pairFailure.clear();state=0;
 auditThisPair=false;movingCaptured=false;auditOrigin={};capturedPair={};for(auto& records:worldInputs)records.clear();
 worldUiSuppressed=0;shaderBindings.clear();for(auto& records:drawShaderRecords)records.clear();
 dispatchRecords.clear();currentPso.clear();skinRenderRecords.clear();characterVertexRecords.clear();
 raceSceneCb=0;sceneAnimationBank=UINT_MAX;animationBankOverrides=0;sceneShadowAddress=discoveredSceneBinding=discoveredSceneCb=0;sceneMapCalls=sceneUnmapCalls=0;driverParamsSeen={};
 hudArmed=hudCopied=false;hudClears=0;hudCopiedFrames=0;hudCaptureResult=E_PENDING;uiSurfaceState=UINT_MAX;
 posePatches=fullBankPairs=fullBankMatrices=0;maxPosePatches=0;poseCollectTicks=poseCollectCalls=0;
 leftTicks=rightTicks=patchTicks=restoreTicks=submitTicks=0;captureResults={E_PENDING,E_PENDING};requestedRecenter=false;
 auto thread=CreateThread(nullptr,0,init,nullptr,0,nullptr);if(!thread){workerBusy=false;state=2;return 3;}CloseHandle(thread);return 0;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){self=h;DisableThreadLibraryCalls(h);auto t=CreateThread(nullptr,0,init,nullptr,0,nullptr);if(t)CloseHandle(t);}return TRUE;}
