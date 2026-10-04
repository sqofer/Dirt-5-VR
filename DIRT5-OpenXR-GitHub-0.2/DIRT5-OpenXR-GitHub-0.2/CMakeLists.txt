cmake_minimum_required(VERSION 3.22)
project(DIRT5OpenXRMod VERSION 0.2 LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Windows x64 and MSVC are required")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(openxr_sdk
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/f2448a8797c85814aa892efc1ab8707900fbcc78.tar.gz)
FetchContent_Declare(minhook
  URL https://github.com/TsudaKageyu/minhook/archive/c3fcafdc10146beb5919319d0683e44e3c30d537.tar.gz)
FetchContent_MakeAvailable(openxr_sdk minhook)

add_library(DIRT5OpenXRMod SHARED src/native_xr_mod.cpp)
target_include_directories(DIRT5OpenXRMod PRIVATE
  "${openxr_sdk_SOURCE_DIR}/include" "${openxr_sdk_SOURCE_DIR}/src/common")
target_compile_definitions(DIRT5OpenXRMod PRIVATE
  XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D12 NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(DIRT5OpenXRMod PRIVATE minhook d3d12 dxgi d3dcompiler windowscodecs ole32 user32)

add_executable(DIRT5VRLauncher WIN32 src/playable_launcher.cpp src/launcher.rc)
target_include_directories(DIRT5VRLauncher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(DIRT5VRLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(DIRT5VRLauncher PRIVATE shell32 comdlg32 dwmapi uxtheme ole32 advapi32)

add_executable(DIRT5ModControl src/mod_controller.cpp)
target_compile_definitions(DIRT5ModControl PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
foreach(target IN ITEMS DIRT5OpenXRMod DIRT5VRLauncher DIRT5ModControl)
  target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc)
endforeach()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/settings.ini" [=[
[OpenXR]
Runtime=system
RuntimeJson=D:\Steam\steamapps\common\SteamVR\steamxr_win64.json
SteamVRJson=D:\Steam\steamapps\common\SteamVR\steamxr_win64.json
EyeWidth=2000
EyeHeight=2000

[Game]
Exe=F:\SteamLibrary\steamapps\common\DIRT 5\DIRT5.exe

[Mod]
DurationSeconds=0
NativeSceneResize=1
FinalOutputLinear=1
DirectFinalOutput=1
IndependentFinalConstants=1
RightMatrixBankSelection=1
StereoSwapLatency=1
MonoMonitor=1
ParameterAudit=0
ActorCpuProbe=0

[HUD]
DistanceMeters=3
]=])
add_dependencies(DIRT5VRLauncher openxr_loader DIRT5OpenXRMod DIRT5ModControl)
add_custom_command(TARGET DIRT5VRLauncher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:openxr_loader>" "$<TARGET_FILE_DIR:DIRT5VRLauncher>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/settings.ini" "$<TARGET_FILE_DIR:DIRT5VRLauncher>/settings.ini")
install(TARGETS DIRT5VRLauncher DIRT5ModControl DIRT5OpenXRMod openxr_loader
  RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/settings.ini" DESTINATION .)
install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME MinHook-LICENSE.txt)
install(FILES "${openxr_sdk_SOURCE_DIR}/COPYING.adoc" DESTINATION licenses RENAME Khronos-COPYING.adoc)
install(DIRECTORY "${openxr_sdk_SOURCE_DIR}/LICENSES/" DESTINATION licenses/Khronos)
