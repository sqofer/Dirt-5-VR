#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
struct Handle{HANDLE h=nullptr;~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}operator HANDLE()const{return h;}};
static UINT64 module(DWORD pid,const std::wstring& name,bool path=false){Handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid)};MODULEENTRY32W e{};e.dwSize=sizeof(e);if(Module32FirstW(snap,&e))do{if(!_wcsicmp(path?e.szExePath:e.szModule,name.c_str()))return reinterpret_cast<UINT64>(e.modBaseAddr);}while(Module32NextW(snap,&e));return 0;}
static DWORD call(HANDLE p,UINT64 address,void* argument=nullptr){Handle t{CreateRemoteThread(p,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(address),argument,0,nullptr)};if(!t.h)throw std::runtime_error("Remote control thread failed");if(WaitForSingleObject(t,15000)!=WAIT_OBJECT_0)throw std::runtime_error("Remote call timeout; process left running");DWORD code=0;if(!GetExitCodeThread(t,&code))throw std::runtime_error("Remote result unavailable");return code;}
int wmain(int argc,wchar_t** argv){try{
 auto folder=std::filesystem::absolute(std::filesystem::path(argv[0]).parent_path());DWORD pid=0;bool wait=false;std::wstring action=L"start",dllName=L"DIRT5OpenXRMod.dll";UINT seconds=0;
 for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a==L"--attach"&&i+1<argc)pid=std::stoul(argv[++i]);else if(a==L"--seconds"&&i+1<argc)seconds=std::stoul(argv[++i]);else if(a==L"--module"&&i+1<argc)dllName=argv[++i];else if(a==L"--wait")wait=true;else if(a==L"--stop")action=L"Stop";else if(a==L"--recenter")action=L"Recenter";else throw std::runtime_error("Usage: --attach PID [--seconds N] [--wait] [--stop | --recenter] [--module NAME]");}
 if(!pid||seconds>3600||(wait&&!seconds))throw std::runtime_error("Invalid PID or duration; --wait requires a nonzero duration");
 Handle process{OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD,FALSE,pid)};if(!process.h)throw std::runtime_error("Game process unavailable");
 wchar_t actual[32768]{},expected[32768]{};DWORD size=32768;GetPrivateProfileStringW(L"Game",L"Exe",L"F:\\SteamLibrary\\steamapps\\common\\DIRT 5\\DIRT5.exe",expected,32768,(folder/L"settings.ini").c_str());if(!QueryFullProcessImageNameW(process,0,actual,&size)||_wcsicmp(actual,expected))throw std::runtime_error("Exact DIRT5 path mismatch");
 if(action!=L"start"){auto name=L"Local\\DIRT5OpenXR."+action+L"."+std::to_wstring(pid);Handle e{OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str())};if(!e.h||!SetEvent(e))throw std::runtime_error("VR control event not active");std::wcout<<action<<L" requested for PID "<<pid<<L'\n';return 0;}
 if(std::filesystem::path(dllName).filename()!=dllName||dllName.find(L"DIRT5")!=0)throw std::runtime_error("Module must be a package DLL filename");auto dll=folder/dllName;if(!std::filesystem::is_regular_file(dll))throw std::runtime_error("Adapter DLL missing");
 UINT64 game=module(pid,L"DIRT5.exe");const BYTE wanted[]={0x48,0x8b,0xc4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x18,0xfe,0xff,0xff};BYTE got[sizeof(wanted)]{};SIZE_T bytes=0;
 if(!game||!ReadProcessMemory(process,reinterpret_cast<void*>(game+0x34b1c0),got,sizeof(got),&bytes)||bytes!=sizeof(got)||memcmp(wanted,got,sizeof(got)))throw std::runtime_error("Native frame version mismatch or another hook is active");
 if(!WritePrivateProfileStringW(L"Mod",L"DurationSeconds",std::to_wstring(seconds).c_str(),(folder/L"settings.ini").c_str()))throw std::runtime_error("Mod duration write failed");
 UINT64 remote=module(pid,dll.wstring(),true);bool restart=remote!=0;
 if(!remote){
  auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;if(!load||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner))throw std::runtime_error("Loader owner missing");wchar_t ownerPath[32768]{};GetModuleFileNameW(owner,ownerPath,32768);auto loaderBase=module(pid,std::filesystem::path(ownerPath).filename().wstring());if(!loaderBase)throw std::runtime_error("Remote loader missing");
  auto text=dll.wstring();SIZE_T length=(text.size()+1)*sizeof(wchar_t);void* buffer=VirtualAllocEx(process,nullptr,length,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!buffer)throw std::runtime_error("DLL path allocation failed");
  if(!WriteProcessMemory(process,buffer,text.c_str(),length,&bytes)||bytes!=length){VirtualFreeEx(process,buffer,0,MEM_RELEASE);throw std::runtime_error("DLL path write failed");}
  // A timed-out thread may still read its argument. Keep that allocation on timeout.
  call(process,loaderBase+reinterpret_cast<UINT64>(load)-reinterpret_cast<UINT64>(owner),buffer);VirtualFreeEx(process,buffer,0,MEM_RELEASE);remote=module(pid,dll.wstring(),true);if(!remote)throw std::runtime_error("Adapter did not load");
 }
 auto local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);if(!local)throw std::runtime_error("Export map unavailable");
 auto exportAddress=[&](const char* name){auto f=GetProcAddress(local,name);if(!f)throw std::runtime_error("Adapter export missing");return remote+reinterpret_cast<UINT64>(f)-reinterpret_cast<UINT64>(local);};
 auto ready=exportAddress("DIRT5ModReady");if(restart&&call(process,exportAddress("DIRT5ModRestart")))throw std::runtime_error("Adapter restart refused");FreeLibrary(local);
 auto deadline=GetTickCount64()+75000;DWORD state=0;while(GetTickCount64()<deadline&&(state=call(process,ready))==0)Sleep(100);if(state!=1&&state!=3)throw std::runtime_error("Adapter startup failed; inspect native-xr-mod log");
 std::cout<<"ADAPTER_READY PID="<<pid<<" seconds="<<seconds<<"\n";
 if(wait){if(!seconds)throw std::runtime_error("--wait requires a bounded duration");deadline=GetTickCount64()+seconds*1000+15000;while(GetTickCount64()<deadline&&(state=call(process,ready))==1)Sleep(100);if(state!=3)throw std::runtime_error("Adapter test failed or still active");std::cout<<"ADAPTER_TEST_PASS\n";}
 return 0;
}catch(const std::exception& e){std::cerr<<"ADAPTER_CONTROL_FAIL: "<<e.what()<<'\n';return 1;}}
