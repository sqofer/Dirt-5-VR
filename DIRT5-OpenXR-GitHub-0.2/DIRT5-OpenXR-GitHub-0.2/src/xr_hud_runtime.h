#pragma once
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr_hud_blit.h"
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
using Microsoft::WRL::ComPtr;
namespace dirt5_xr_pair {
static void hr(HRESULT h){if(FAILED(h))throw std::runtime_error("XR binding HRESULT="+std::to_string(h));}
static void xr(XrResult r,const char* call){if(XR_FAILED(r))throw std::runtime_error(std::string(call)+" XrResult="+std::to_string(r));}
#define XR_FUNCTIONS(X) \
 X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
 X(xrGetD3D12GraphicsRequirementsKHR) X(xrCreateSession) X(xrDestroySession) \
 X(xrEnumerateViewConfigurationViews) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
 X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrCreateReferenceSpace) X(xrDestroySpace) \
 X(xrEnumerateEnvironmentBlendModes) X(xrPollEvent) X(xrBeginSession) X(xrEndSession) \
 X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrLocateSpace) X(xrWaitFrame) X(xrBeginFrame) X(xrLocateViews) X(xrEndFrame) X(xrRequestExitSession)
struct Api {
    HMODULE loader=nullptr;PFN_xrGetInstanceProcAddr get=nullptr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties=nullptr;PFN_xrCreateInstance xrCreateInstance=nullptr;
#define MEMBER(name) PFN_##name name=nullptr;
    XR_FUNCTIONS(MEMBER)
#undef MEMBER
    ~Api(){if(loader)FreeLibrary(loader);}
    template<class T>void resolve(XrInstance instance,const char* name,T& target){xr(get(instance,name,reinterpret_cast<PFN_xrVoidFunction*>(&target)),name);if(!target)throw std::runtime_error("Null OpenXR function");}
    void setup(const std::filesystem::path& folder){
        loader=LoadLibraryExW((folder/L"openxr_loader.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!loader)throw std::runtime_error("Official loader unavailable");get=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader,"xrGetInstanceProcAddr"));if(!get)throw std::runtime_error("Loader entry missing");
        resolve(XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",xrEnumerateInstanceExtensionProperties);resolve(XR_NULL_HANDLE,"xrCreateInstance",xrCreateInstance);
    }
    void load(XrInstance instance){
#define RESOLVE(name) resolve(instance,#name,name);
        XR_FUNCTIONS(RESOLVE)
#undef RESOLVE
    }
};
struct Context {
    Api& api;XrInstance instance=XR_NULL_HANDLE;XrSession session=XR_NULL_HANDLE;XrSpace space=XR_NULL_HANDLE,headSpace=XR_NULL_HANDLE;std::array<XrSwapchain,3> swaps{};
    ~Context(){for(auto swap:swaps)if(swap&&api.xrDestroySwapchain)api.xrDestroySwapchain(swap);if(headSpace&&api.xrDestroySpace)api.xrDestroySpace(headSpace);if(space&&api.xrDestroySpace)api.xrDestroySpace(space);if(session&&api.xrDestroySession)api.xrDestroySession(session);if(instance&&api.xrDestroyInstance)api.xrDestroyInstance(instance);}
};
struct Environment {
    std::vector<std::pair<std::wstring,std::wstring>> saved;std::vector<bool> existed;
    void set(const wchar_t* name,const std::wstring& value){SetLastError(0);DWORD n=GetEnvironmentVariableW(name,nullptr,0);bool found=n||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;std::wstring old(n? n:1,L'\0');if(n){GetEnvironmentVariableW(name,old.data(),n);old.resize(n-1);}else old.clear();saved.emplace_back(name,old);existed.push_back(found);if(!SetEnvironmentVariableW(name,value.c_str()))throw std::runtime_error("Environment setup failed");}
    void restore(){for(size_t i=saved.size();i>0;--i)SetEnvironmentVariableW(saved[i-1].first.c_str(),existed[i-1]?saved[i-1].second.c_str():nullptr);saved.clear();existed.clear();}
    ~Environment(){restore();}
};

class Runtime {
 Api api;Context c{api};Environment env;
 std::array<std::vector<XrSwapchainImageD3D12KHR>,3> images;
 std::array<UINT,3> imageCounts{},indices{};std::array<bool,3> acquired{},waited{};
 XrEnvironmentBlendMode blend=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
 XrFrameState frame{XR_TYPE_FRAME_STATE};bool running=false,frameOpen=false,referenceValid=false,exitRequested=false;UINT frameEyes=2;
 XrHudBlit* blit=nullptr;bool hudVisible=false;float hudAspect=2560.f/1080.f,hudDistance=3.f;
 void empty(){XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=frame.predictedDisplayTime;end.environmentBlendMode=blend;xr(api.xrEndFrame(c.session,&end),"End empty frame");frameOpen=false;}
 void waitBegin(){XrFrameWaitInfo w{XR_TYPE_FRAME_WAIT_INFO};frame={XR_TYPE_FRAME_STATE};xr(api.xrWaitFrame(c.session,&w,&frame),"Wait frame");XrFrameBeginInfo b{XR_TYPE_FRAME_BEGIN_INFO};xr(api.xrBeginFrame(c.session,&b),"Begin frame");frameOpen=true;}
 void layer(){
  std::array<XrCompositionLayerProjectionView,2> pv{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
  for(UINT eye=0;eye<2;++eye){pv[eye].pose=eyes[eye].pose;pv[eye].fov=eyes[eye].fov;pv[eye].subImage.swapchain=c.swaps[eye];pv[eye].subImage.imageRect.extent={static_cast<int32_t>(width),static_cast<int32_t>(height)};}
  XrCompositionLayerProjection p{XR_TYPE_COMPOSITION_LAYER_PROJECTION};p.space=c.space;p.viewCount=2;p.views=pv.data();XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};hud.space=c.headSpace;hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;hud.eyeVisibility=XR_EYE_VISIBILITY_BOTH;hud.pose.orientation.w=1;hud.pose.position.z=-hudDistance;float hudWidth=1.2f*hudDistance;hud.size={hudWidth,hudWidth/hudAspect};hud.subImage.swapchain=c.swaps[2];hud.subImage.imageRect.extent={static_cast<int32_t>(width),static_cast<int32_t>(height)};
  const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&p),reinterpret_cast<XrCompositionLayerBaseHeader*>(&hud)};
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=frame.predictedDisplayTime;end.environmentBlendMode=blend;end.layerCount=hudVisible?2:1;end.layers=layers;xr(api.xrEndFrame(c.session,&end),"Submit native stereo projection and HUD layers");frameOpen=false;
 }
public:
 UINT width=0,height=0;std::array<XrView,2> eyes{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};XrPosef headCentre{};XrTime renderedTime=0;bool submitted=false;UINT heldFrames=0;
 void setup(const std::filesystem::path& folder,ID3D12CommandQueue* queue,std::ofstream& log){
  auto settings=(folder/L"settings.ini").wstring();ComPtr<ID3D12CommandQueue> nativeQueue=queue;ComPtr<ID3D12Device> device;hr(queue->GetDevice(IID_PPV_ARGS(&device)));
        width=GetPrivateProfileIntW(L"OpenXR",L"EyeWidth",2000,settings.c_str());height=GetPrivateProfileIntW(L"OpenXR",L"EyeHeight",2000,settings.c_str());
        if(width<256||height<256||width>8192||height>8192)throw std::runtime_error("Invalid eye texture dimensions");
        wchar_t distanceText[64]{};GetPrivateProfileStringW(L"HUD",L"DistanceMeters",L"3",distanceText,64,settings.c_str());std::wstring distance(distanceText);std::replace(distance.begin(),distance.end(),L',',L'.');try{size_t used=0;hudDistance=std::stof(distance,&used);if(used!=distance.size()||!std::isfinite(hudDistance)||hudDistance<.5f||hudDistance>10.f)throw std::runtime_error("range");}catch(...){throw std::runtime_error("HUD distance must be from 0.5 to 10 meters");}log<<"HUD distance="<<hudDistance<<" meters"<<std::endl;
        wchar_t mode[64]{},runtime[32768]{};GetPrivateProfileStringW(L"OpenXR",L"Runtime",L"steamvr",mode,64,settings.c_str());GetPrivateProfileStringW(L"OpenXR",L"RuntimeJson",L"",runtime,32768,settings.c_str());
        if(_wcsicmp(mode,L"system")){if(!std::filesystem::is_regular_file(runtime))throw std::runtime_error("Runtime JSON missing");env.set(L"XR_RUNTIME_JSON",runtime);}
        env.set(L"XRFG_DISABLE_OFXR_BRIDGE",L"1");env.set(L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit",L"1");env.set(L"DISABLE_XR_APILAYER_DesktopXR",L"1");
        api.setup(folder);uint32_t count=0;xr(api.xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr),"Enumerate extensions");std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});xr(api.xrEnumerateInstanceExtensionProperties(nullptr,count,&count,extensions.data()),"Enumerate extensions");
        if(std::none_of(extensions.begin(),extensions.end(),[](const auto& e){return !strcmp(e.extensionName,XR_KHR_D3D12_ENABLE_EXTENSION_NAME);}))throw std::runtime_error("Runtime lacks D3D12 extension");
        const char* enabled[]={XR_KHR_D3D12_ENABLE_EXTENSION_NAME};XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};strcpy_s(ici.applicationInfo.applicationName,"DIRT5 OpenXR Mod");ici.applicationInfo.apiVersion=XR_API_VERSION_1_0;ici.enabledExtensionCount=1;ici.enabledExtensionNames=enabled;
        xr(api.xrCreateInstance(&ici,&c.instance),"Create instance");api.load(c.instance);XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};xr(api.xrGetInstanceProperties(c.instance,&properties),"Runtime properties");log<<"Runtime="<<properties.runtimeName<<std::endl;
        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};sgi.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;XrSystemId system=0;xr(api.xrGetSystem(c.instance,&sgi,&system),"Get headset system");
        XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};xr(api.xrGetSystemProperties(c.instance,system,&systemProperties),"System properties");log<<"Headset="<<systemProperties.systemName<<std::endl;
        XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};xr(api.xrGetD3D12GraphicsRequirementsKHR(c.instance,system,&requirements),"Graphics requirements");auto luid=device->GetAdapterLuid();if(memcmp(&luid,&requirements.adapterLuid,sizeof(luid)))throw std::runtime_error("Game GPU does not match runtime GPU");
        D3D_FEATURE_LEVEL levels[]={requirements.minFeatureLevel};D3D12_FEATURE_DATA_FEATURE_LEVELS feature{1,levels,D3D_FEATURE_LEVEL_1_0_CORE};hr(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&feature,sizeof(feature)));
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};binding.device=device.Get();binding.queue=nativeQueue.Get();XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};sci.next=&binding;sci.systemId=system;xr(api.xrCreateSession(c.instance,&sci,&c.session),"Create native-device session");
        xr(api.xrEnumerateViewConfigurationViews(c.instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"View count");if(count!=2)throw std::runtime_error("Exactly two primary views required");
        std::array<XrViewConfigurationView,2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};xr(api.xrEnumerateViewConfigurationViews(c.instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data()),"View limits");for(auto& v:views)if(width>v.maxImageRectWidth||height>v.maxImageRectHeight)throw std::runtime_error("Eye dimensions exceed runtime limits");
        xr(api.xrEnumerateSwapchainFormats(c.session,0,&count,nullptr),"Format count");std::vector<int64_t> formats(count);xr(api.xrEnumerateSwapchainFormats(c.session,count,&count,formats.data()),"Formats");
        log<<"RuntimeFormats=";for(auto format:formats)log<<format<<',';log<<std::endl;
        int64_t chosen=0;for(auto candidate:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM})if(!chosen&&std::find(formats.begin(),formats.end(),candidate)!=formats.end())chosen=candidate;
        if(!chosen)throw std::runtime_error("No supported 8-bit color swapchain format");log<<"SelectedFormat="<<chosen<<std::endl;
        
        if(systemProperties.graphicsProperties.maxLayerCount<2)throw std::runtime_error("Runtime must support scene and HUD layers");
        for(UINT eye=0;eye<3;++eye){XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;info.format=chosen;info.sampleCount=1;info.width=width;info.height=height;info.faceCount=1;info.arraySize=1;info.mipCount=1;xr(api.xrCreateSwapchain(c.session,&info,&c.swaps[eye]),"Create native-device eye swapchain");
            xr(api.xrEnumerateSwapchainImages(c.swaps[eye],0,&count,nullptr),"Image count");images[eye].assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});xr(api.xrEnumerateSwapchainImages(c.swaps[eye],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images[eye].data())),"Native images");imageCounts[eye]=count;if(!count)throw std::runtime_error("Empty swapchain");
            for(auto& image:images[eye]){auto d=image.texture->GetDesc();if(d.Width!=width||d.Height!=height)throw std::runtime_error("Native texture dimensions differ");ComPtr<ID3D12Device> owner;hr(image.texture->GetDevice(IID_PPV_ARGS(&owner)));ComPtr<IUnknown> a,b;hr(owner.As(&a));hr(device.As(&b));if(a.Get()!=b.Get())throw std::runtime_error("XR image belongs to another device");}
        }
        log<<"NATIVE_BINDING_PASS: host device/queue; two eye swapchains "<<width<<'x'<<height<<"; preparing a bounded native stereo pair"<<std::endl;
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};spaceInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;spaceInfo.poseInReferenceSpace.orientation.w=1;
        xr(api.xrCreateReferenceSpace(c.session,&spaceInfo,&c.space),"Create local space");spaceInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;xr(api.xrCreateReferenceSpace(c.session,&spaceInfo,&c.headSpace),"Create head view space");
        xr(api.xrEnumerateEnvironmentBlendModes(c.instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"Blend count");std::vector<XrEnvironmentBlendMode> blends(count);xr(api.xrEnumerateEnvironmentBlendModes(c.instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,count,&count,blends.data()),"Blends");if(blends.empty())throw std::runtime_error("No environment blend mode");
        blend=std::find(blends.begin(),blends.end(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)!=blends.end()?XR_ENVIRONMENT_BLEND_MODE_OPAQUE:blends.front();

  blit=new XrHudBlit;blit->setup(device.Get(),queue,width,height,static_cast<DXGI_FORMAT>(chosen));
  auto deadline=GetTickCount64()+10000;
  while(!running&&GetTickCount64()<deadline){XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};auto result=api.xrPollEvent(c.instance,&event);if(result==XR_EVENT_UNAVAILABLE){Sleep(10);continue;}xr(result,"Poll initial event");
   if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){auto change=reinterpret_cast<XrEventDataSessionStateChanged*>(&event);log<<"SessionState="<<change->state<<std::endl;
    if(change->state==XR_SESSION_STATE_READY){XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO};b.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;xr(api.xrBeginSession(c.session,&b),"Begin native session");running=true;}}
  }
  if(!running)throw std::runtime_error("XR session did not become ready");
  waitBegin();empty();log<<"XR_READY: game device/queue, acquired image state contract, projection submission prepared"<<std::endl;
 }
 void recenter(){referenceValid=false;}
 bool events(){
  for(;;){XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};auto result=api.xrPollEvent(c.instance,&event);if(result==XR_EVENT_UNAVAILABLE)break;xr(result,"Poll session event");
   if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING){exitRequested=true;return false;}
   if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)referenceValid=false;
   if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){auto change=reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
    if(change->state==XR_SESSION_STATE_READY&&!running){XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO};b.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;xr(api.xrBeginSession(c.session,&b),"Begin resumed session");running=true;referenceValid=false;}
    if(change->state==XR_SESSION_STATE_STOPPING&&running){xr(api.xrEndSession(c.session),"End stopping session");running=false;}
    if(change->state==XR_SESSION_STATE_EXITING||change->state==XR_SESSION_STATE_LOSS_PENDING){exitRequested=true;return false;}
   }
  }return running&&!exitRequested;
 }
 bool exiting()const{return exitRequested;}
 bool begin(bool stereo=true){
  if(!events())return false;frameEyes=stereo?2:1;
  waitBegin();if(!frame.shouldRender){empty();return false;}
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;locate.displayTime=frame.predictedDisplayTime;locate.space=c.space;XrViewState vs{XR_TYPE_VIEW_STATE};uint32_t count=0;
  xr(api.xrLocateViews(c.session,&locate,&vs,2,&count,eyes.data()),"Locate pair at render time");XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};xr(api.xrLocateSpace(c.headSpace,c.space,frame.predictedDisplayTime,&head),"Locate recenter head");
  if(count!=2||!(vs.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)||!(vs.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT)||!(head.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)||!(head.locationFlags&XR_SPACE_LOCATION_POSITION_VALID_BIT)){empty();return false;}
  if(!referenceValid){headCentre=head.pose;referenceValid=true;}renderedTime=frame.predictedDisplayTime;
  for(UINT eye=0;eye<frameEyes;++eye){XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};xr(api.xrAcquireSwapchainImage(c.swaps[eye],&a,&indices[eye]),"Acquire eye image");acquired[eye]=true;
   XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=2000000000;auto result=api.xrWaitSwapchainImage(c.swaps[eye],&w);if(result!=XR_SUCCESS)throw std::runtime_error("XR eye wait did not complete");waited[eye]=true;}
  if(stereo){XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};xr(api.xrAcquireSwapchainImage(c.swaps[2],&a,&indices[2]),"Acquire HUD image");acquired[2]=true;XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=2000000000;auto result=api.xrWaitSwapchainImage(c.swaps[2],&w);if(result!=XR_SUCCESS)throw std::runtime_error("HUD image wait did not complete");waited[2]=true;}
  return true;
 }
 void submit(const std::array<ID3D12Resource*,2>& sources,ID3D12Resource* hud=nullptr,bool projectionSourcesLinear=false){
  std::array<ID3D12Resource*,3> destinations{};for(UINT eye=0;eye<2;++eye){if(!acquired[eye]||!waited[eye])throw std::runtime_error("Both acquired eyes required");destinations[eye]=images[eye].at(indices[eye]).texture;}
  hudVisible=hud!=nullptr;if(hudVisible){if(!acquired[2]||!waited[2])throw std::runtime_error("HUD image unavailable");destinations[2]=images[2].at(indices[2]).texture;auto d=hud->GetDesc();hudAspect=static_cast<float>(d.Width)/d.Height;}
  blit->transfer({sources[0],sources[1],hud},destinations,hudVisible?3:2,projectionSourcesLinear);
  for(UINT eye=0;eye<2;++eye){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr(api.xrReleaseSwapchainImage(c.swaps[eye],&r),"Release completed eye image in RT state");acquired[eye]=waited[eye]=false;}
  if(acquired[2]){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr(api.xrReleaseSwapchainImage(c.swaps[2],&r),"Release HUD image");acquired[2]=waited[2]=false;}
  layer();submitted=true;
 }
 void submitCinema(ID3D12Resource* source){
  if(frameEyes!=1||!frameOpen||!acquired[0]||!waited[0]||!source)throw std::runtime_error("Cinema frame unavailable");
  auto desc=source->GetDesc();if(!desc.Width||!desc.Height)throw std::runtime_error("Cinema source dimensions unavailable");
  blit->transfer({source,nullptr},{images[0].at(indices[0]).texture,nullptr},1);
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr(api.xrReleaseSwapchainImage(c.swaps[0],&release),"Release cinema image");acquired[0]=waited[0]=false;
  XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};q.space=c.headSpace;q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;q.pose.orientation.w=1;q.pose.position.z=-3;
  q.size={4,4*static_cast<float>(desc.Height)/static_cast<float>(desc.Width)};q.subImage.swapchain=c.swaps[0];q.subImage.imageRect.extent={static_cast<int32_t>(width),static_cast<int32_t>(height)};
  const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q)};
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=frame.predictedDisplayTime;end.environmentBlendMode=blend;end.layerCount=1;end.layers=layers;
  xr(api.xrEndFrame(c.session,&end),"Submit cinema quad");frameOpen=false;submitted=true;
 }
 void holdFrozenPair(UINT milliseconds){
  auto end=GetTickCount64()+milliseconds;while(submitted&&GetTickCount64()<end){waitBegin();if(frame.shouldRender){layer();++heldFrames;}else empty();}
 }
 void abort(){
  if(blit&&!blit->idle())throw std::runtime_error("XR resources retained because GPU transfer is pending");
  // Only used when no transfer was submitted or its GPU fence completed.
  for(UINT eye=0;eye<3;++eye)if(acquired[eye]){if(!waited[eye]){XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=2000000000;auto result=api.xrWaitSwapchainImage(c.swaps[eye],&w);if(result!=XR_SUCCESS)throw std::runtime_error("Abort image wait incomplete");waited[eye]=true;}XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr(api.xrReleaseSwapchainImage(c.swaps[eye],&r),"Abort release");acquired[eye]=waited[eye]=false;}
  if(frameOpen)empty();
 }
 void close(std::ofstream& log){
  abort();if(c.session&&running){xr(api.xrRequestExitSession(c.session),"Request exit");auto deadline=GetTickCount64()+3000;
   while(running&&GetTickCount64()<deadline){XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};auto result=api.xrPollEvent(c.instance,&event);if(result==XR_EVENT_UNAVAILABLE){Sleep(10);continue;}xr(result,"Poll exit event");
    if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED&&reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state==XR_SESSION_STATE_STOPPING){xr(api.xrEndSession(c.session),"End native session");running=false;}}
  }
  for(auto& swap:c.swaps)if(swap){xr(api.xrDestroySwapchain(swap),"Destroy eye swapchain");swap=XR_NULL_HANDLE;}
  if(c.headSpace){xr(api.xrDestroySpace(c.headSpace),"Destroy head space");c.headSpace=XR_NULL_HANDLE;}if(c.space){xr(api.xrDestroySpace(c.space),"Destroy local space");c.space=XR_NULL_HANDLE;}
  if(c.session){xr(api.xrDestroySession(c.session),"Destroy session");c.session=XR_NULL_HANDLE;running=false;}if(c.instance){xr(api.xrDestroyInstance(c.instance),"Destroy instance");c.instance=XR_NULL_HANDLE;}
  if(blit){delete blit;blit=nullptr;}
  if(api.loader){FreeLibrary(api.loader);api.loader=nullptr;}env.restore();log<<"CLOSED: XR objects destroyed, loader reference released, process environment restored"<<std::endl;
 }
 void restoreEnvironment(){env.restore();}
};
} // namespace dirt5_xr_pair
#undef XR_FUNCTIONS
