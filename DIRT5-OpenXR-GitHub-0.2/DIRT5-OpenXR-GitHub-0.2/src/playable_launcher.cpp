#include <windows.h>
#include "launcher_resources.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <commdlg.h>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include "game_profile_options.h"

static std::wstring folder,settings,gamePath;
static HWND window,runtimeBox,jsonBox,widthBox,heightBox,hudDistanceBox,statusBox,startButton;
static HFONT font,headingFont,supportFont,storeFont;static HBRUSH backgroundBrush,fieldBrush;
static constexpr COLORREF background=RGB(24,24,24),field=RGB(38,38,38),foreground=RGB(235,235,235),muted=RGB(160,160,160);static std::atomic<bool> busy{false};static DWORD attachedPid=0;
enum{ID_RUNTIME=100,ID_JSON,ID_WIDTH,ID_HEIGHT,ID_SAVE,ID_START,ID_STOP,ID_CENTER,ID_FOLDER,ID_BROWSE,ID_HUD_DISTANCE,ID_SUPPORT_STEAM,ID_SUPPORT_QUEST,ID_SUPPORT_LINE,ID_SUPPORT_TITLE,ID_SUPPORT_DESCRIPTION};
constexpr UINT WM_RESULT=WM_APP+1;
static constexpr const wchar_t* steamStoreUrl=L"https://store.steampowered.com/app/1636850/CYBRID/";
static constexpr const wchar_t* questStoreUrl=L"https://www.meta.com/en-gb/experiences/cybrid/24008454042113681/";
static std::wstring setting(const wchar_t* section,const wchar_t* key,const wchar_t* fallback){std::vector<wchar_t> b(32768);GetPrivateProfileStringW(section,key,fallback,b.data(),static_cast<DWORD>(b.size()),settings.c_str());return b.data();}
static std::wstring steamRuntimeJson(){
 auto saved=setting(L"OpenXR",L"SteamVRJson",L"");if(std::filesystem::is_regular_file(saved))return saved;
 wchar_t steam[32768]{};DWORD bytes=sizeof(steam);
 if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,steam,&bytes)==ERROR_SUCCESS){auto candidate=std::filesystem::path(steam)/L"steamapps"/L"common"/L"SteamVR"/L"steamxr_win64.json";if(std::filesystem::is_regular_file(candidate))return candidate.wstring();}
 return saved;
}
static std::wstring value(HWND h){int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
static void status(const std::wstring& s){SetWindowTextW(statusBox,s.c_str());}
static void notify(const std::wstring& s,DWORD pid=0){auto p=new std::wstring(s);if(!PostMessageW(window,WM_RESULT,pid,reinterpret_cast<LPARAM>(p)))delete p;}
static bool save(){try{
 auto number=[](const std::wstring& s){size_t used=0;int n=std::stoi(s,&used);if(used!=s.size()||n<256||n>8192)throw 1;return n;};
 int width=number(value(widthBox)),height=number(value(heightBox));int selected=static_cast<int>(SendMessageW(runtimeBox,CB_GETCURSEL,0,0));auto mode=selected==0?L"steamvr":selected==1?L"system":L"custom";auto json=value(jsonBox);
 if(selected!=1&&!std::filesystem::is_regular_file(json)){status(L"OpenXR runtime file not found. Select a valid JSON file or the system runtime.");return false;}
 auto distanceText=value(hudDistanceBox);std::replace(distanceText.begin(),distanceText.end(),L',',L'.');float distance=0;try{size_t used=0;distance=std::stof(distanceText,&used);if(used!=distanceText.size()||!std::isfinite(distance)||distance<.5f||distance>10.f)throw 1;}catch(...){status(L"Enter a HUD distance between 0.5 and 10 meters.");return false;}
 for(auto item:std::vector<std::pair<std::wstring,std::wstring>>{{L"Runtime",mode},{L"RuntimeJson",json},{L"EyeWidth",std::to_wstring(width)},{L"EyeHeight",std::to_wstring(height)}})
  if(!WritePrivateProfileStringW(L"OpenXR",item.first.c_str(),item.second.c_str(),settings.c_str()))throw 1;
 if(!WritePrivateProfileStringW(L"Mod",L"DurationSeconds",L"0",settings.c_str()))throw 1;
 if(!WritePrivateProfileStringW(L"HUD",L"DistanceMeters",distanceText.c_str(),settings.c_str()))throw 1;
 status(L"Settings saved. To apply changes, click Stop VR, then Launch / Start VR.");return true;
 }catch(...){status(L"Enter whole numbers between 256 and 8192 pixels for each eye dimension.");return false;}}
static bool event(DWORD pid,const wchar_t* action,bool signal){auto name=L"Local\\DIRT5OpenXR."+std::wstring(action)+L"."+std::to_wstring(pid);HANDLE h=OpenEventW(signal?EVENT_MODIFY_STATE:SYNCHRONIZE,FALSE,name.c_str());if(!h)return false;bool ok=!signal||SetEvent(h);CloseHandle(h);return ok;}
static bool graphicsReady(DWORD pid,HANDLE process){
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(snapshot==INVALID_HANDLE_VALUE)return false;MODULEENTRY32W m{};m.dwSize=sizeof(m);UINT64 base=0;
 if(Module32FirstW(snapshot,&m))do{if(!_wcsicmp(m.szModule,L"DIRT5.exe")){base=reinterpret_cast<UINT64>(m.modBaseAddr);break;}}while(Module32NextW(snapshot,&m));CloseHandle(snapshot);if(!base)return false;
 auto pointer=[&](UINT64 address){UINT64 p=0;SIZE_T got=0;return ReadProcessMemory(process,reinterpret_cast<void*>(address),&p,8,&got)&&got==8?p:0;};
 UINT64 wrapper=pointer(base+0x10e7330),queue=pointer(base+0x10da558);return wrapper&&pointer(wrapper+0x78)&&queue&&pointer(queue);
}
static DWORD findGame(bool ready){HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return 0;PROCESSENTRY32W e{};e.dwSize=sizeof(e);DWORD result=0;
 if(Process32FirstW(snapshot,&e))do{if(_wcsicmp(e.szExeFile,L"DIRT5.exe"))continue;HANDLE p=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,e.th32ProcessID);if(!p)continue;wchar_t path[32768]{};DWORD n=32768;
  if(QueryFullProcessImageNameW(p,0,path,&n)&&!_wcsicmp(path,gamePath.c_str())&&(!ready||event(e.th32ProcessID,L"Stop",false)||graphicsReady(e.th32ProcessID,p)))result=e.th32ProcessID;CloseHandle(p);
 }while(Process32NextW(snapshot,&e));CloseHandle(snapshot);return result;}
static DWORD WINAPI start(void*){
 DWORD pid=findGame(true);if(pid&&event(pid,L"Stop",false)){notify(L"VR is already running. Menus use a cinema screen; select cockpit view for racing.",pid);busy=false;return 0;}
 if(!findGame(false)){
  PWSTR documents=nullptr;
  if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&documents))){notify(L"Could not locate the DIRT 5 profile to disable opponent names.");busy=false;return 1;}
  auto profile=std::filesystem::path(documents)/L"My Games"/L"DIRT5"/L"user0"/L"profile"/L"settings.json";CoTaskMemFree(documents);
  try{disableDriverNames(profile);}catch(...){notify(L"Could not disable opponent names in the game profile. The original profile has been preserved; check the profile folder for details.");busy=false;return 1;}
  SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"open";launch.lpFile=gamePath.c_str();auto dir=std::filesystem::path(gamePath).parent_path().wstring();launch.lpDirectory=dir.c_str();launch.nShow=SW_SHOWNORMAL;
  if(!ShellExecuteExW(&launch)){notify(L"Could not launch DIRT 5.");busy=false;return 1;}if(launch.hProcess)CloseHandle(launch.hProcess);
 }
 auto deadline=GetTickCount64()+95000;while(!(pid=findGame(true))&&GetTickCount64()<deadline)Sleep(500);
 if(!pid){notify(L"The game is not ready yet. Wait for the menu, then click Launch / Start VR again.");busy=false;return 1;}
 auto exe=folder+L"\\DIRT5ModControl.exe";std::wstring command=L"\""+exe+L"\" --attach "+std::to_wstring(pid)+L" --seconds 0";
 STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
 if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,folder.c_str(),&si,&pi)){notify(L"Could not attach the mod. Make sure DIRT5ModControl.exe is present.");busy=false;return 1;}
 CloseHandle(pi.hThread);DWORD waited=WaitForSingleObject(pi.hProcess,120000),code=1;if(waited==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);
 if(waited!=WAIT_OBJECT_0)notify(L"Attachment is still in progress. Check the traces folder for status.",pid);
 else if(code==0)notify(L"VR is running. Menus: cinema screen; cockpit racing: VR.\r\nPress Ctrl in the game to recenter. Closing the launcher keeps VR running.",pid);
 else notify(L"Could not attach the mod. Connect your headset and start the selected OpenXR runtime. Check the traces folder for details.",pid);
 busy=false;return code;
}
static HWND control(const wchar_t* cls,const wchar_t* title,DWORD style,int x,int y,int width,int height,int id=0){
 if(!_wcsicmp(cls,L"BUTTON"))style|=BS_OWNERDRAW;
 if(!_wcsicmp(cls,L"EDIT")||!_wcsicmp(cls,L"COMBOBOX"))style|=WS_TABSTOP;
 HWND h=CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
 SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
 if(!_wcsicmp(cls,L"BUTTON")||!_wcsicmp(cls,L"COMBOBOX")||!_wcsicmp(cls,L"EDIT"))SetWindowTheme(h,L"",L"");
 return h;
}
static void drawControl(const DRAWITEMSTRUCT* item){
 FillRect(item->hDC,&item->rcItem,backgroundBrush);
 if(item->CtlType==ODT_STATIC){
  if(item->CtlID==ID_SUPPORT_LINE){auto brush=CreateSolidBrush(RGB(85,85,85));FillRect(item->hDC,&item->rcItem,brush);DeleteObject(brush);return;}
  auto previousFont=SelectObject(item->hDC,supportFont);int previousSpacing=SetTextCharacterExtra(item->hDC,1);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,foreground);
  wchar_t label[256]{};GetWindowTextW(item->hwndItem,label,256);RECT rect=item->rcItem;DrawTextW(item->hDC,label,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  SetTextCharacterExtra(item->hDC,previousSpacing);SelectObject(item->hDC,previousFont);return;
 }
 bool store=item->CtlID==ID_SUPPORT_STEAM||item->CtlID==ID_SUPPORT_QUEST;
 bool combo=item->CtlType==ODT_COMBOBOX,start=item->CtlID==ID_START;
 bool disabled=(item->itemState&ODS_DISABLED)!=0,pressed=(item->itemState&ODS_SELECTED)!=0;
 COLORREF fill=start?RGB(225,225,225):pressed?RGB(63,63,63):RGB(45,45,45);
 if(disabled)fill=RGB(45,45,45);
 auto brush=CreateSolidBrush(fill);auto pen=CreatePen(PS_SOLID,1,start?fill:RGB(65,65,65));auto oldBrush=SelectObject(item->hDC,brush);auto oldPen=SelectObject(item->hDC,pen);
 RoundRect(item->hDC,item->rcItem.left,item->rcItem.top,item->rcItem.right,item->rcItem.bottom,10,10);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(brush);DeleteObject(pen);
 SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?muted:start?RGB(25,25,25):foreground);auto oldFont=SelectObject(item->hDC,store?storeFont:font);int oldSpacing=SetTextCharacterExtra(item->hDC,store?3:0);
 wchar_t text[2048]{};if(combo&&item->itemID!=UINT_MAX)SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,reinterpret_cast<LPARAM>(text));else GetWindowTextW(item->hwndItem,text,2048);
 RECT rect=item->rcItem;rect.left+=12;rect.right-=12;DrawTextW(item->hDC,text,-1,&rect,DT_SINGLELINE|DT_VCENTER|(combo?DT_LEFT:DT_CENTER));
 if(item->itemState&ODS_FOCUS){InflateRect(&rect,-2,-4);DrawFocusRect(item->hDC,&rect);}SetTextCharacterExtra(item->hDC,oldSpacing);SelectObject(item->hDC,oldFont);
}
static LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM w,LPARAM l){switch(msg){
 case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,backgroundBrush);return 1;}
 case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{auto dc=reinterpret_cast<HDC>(w);auto child=reinterpret_cast<HWND>(l);bool input=msg!=WM_CTLCOLORSTATIC||child==statusBox||child==jsonBox;SetTextColor(dc,foreground);SetBkColor(dc,input?field:background);return reinterpret_cast<LRESULT>(input?fieldBrush:backgroundBrush);}
 case WM_DRAWITEM:drawControl(reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
 case WM_MEASUREITEM:{auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(l);if(item->CtlType==ODT_COMBOBOX){item->itemHeight=29;return TRUE;}break;}
 case WM_RESULT:{auto s=reinterpret_cast<std::wstring*>(l);status(*s);delete s;if(w)attachedPid=static_cast<DWORD>(w);EnableWindow(startButton,TRUE);return 0;}
 case WM_COMMAND:{int id=LOWORD(w);
  if(id==ID_RUNTIME&&HIWORD(w)==CBN_SELCHANGE){int i=static_cast<int>(SendMessageW(runtimeBox,CB_GETCURSEL,0,0));EnableWindow(jsonBox,i!=1);if(i==0)SetWindowTextW(jsonBox,steamRuntimeJson().c_str());}
  if(id==ID_SAVE&&!busy.load())save();
  if(id==ID_START&&!busy.load()&&save()){
   if(!std::filesystem::is_regular_file(gamePath)){status(L"DIRT5.exe not found. Update the Exe path in settings.ini.");break;}
   busy=true;EnableWindow(startButton,FALSE);status(L"Launching the game and starting VR. Wait for loading to finish; your OpenXR runtime must be running.");HANDLE t=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(t)CloseHandle(t);else{busy=false;EnableWindow(startButton,TRUE);status(L"Could not start the launch process.");}
  }
  if(id==ID_STOP||id==ID_CENTER){DWORD pid=attachedPid?attachedPid:findGame(true);bool ok=pid&&event(pid,id==ID_STOP?L"Stop":L"Recenter",true);status(ok?(id==ID_STOP?L"Stopping VR. The game will keep running. Wait a few seconds before restarting VR.":L"Recenter requested. The next frame will set your forward position."):L"No active VR session found.");}
  if(id==ID_SUPPORT_STEAM||id==ID_SUPPORT_QUEST){auto url=id==ID_SUPPORT_STEAM?steamStoreUrl:questStoreUrl;if(reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",url,nullptr,nullptr,SW_SHOWNORMAL))<=32)status(L"Could not open the store link. Please check your default browser.");}
  if(id==ID_FOLDER)ShellExecuteW(h,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
  if(id==ID_BROWSE){wchar_t path[32768]{};OPENFILENAMEW f{};f.lStructSize=sizeof(f);f.hwndOwner=h;f.lpstrTitle=L"Select OpenXR Runtime";f.lpstrFilter=L"OpenXR runtime (*.json)\0*.json\0\0";f.lpstrFile=path;f.nMaxFile=32768;f.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(GetOpenFileNameW(&f)){SetWindowTextW(jsonBox,path);SendMessageW(runtimeBox,CB_SETCURSEL,2,0);EnableWindow(jsonBox,TRUE);}}
  return 0;}
 case WM_CLOSE:DestroyWindow(h);return 0;
 case WM_DESTROY:if(font)DeleteObject(font);if(headingFont)DeleteObject(headingFont);if(supportFont)DeleteObject(supportFont);if(storeFont)DeleteObject(storeFont);DeleteObject(backgroundBrush);DeleteObject(fieldBrush);PostQuitMessage(0);return 0;
 }return DefWindowProcW(h,msg,w,l);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
 wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);folder=std::filesystem::path(path).parent_path().wstring();settings=folder+L"\\settings.ini";gamePath=setting(L"Game",L"Exe",L"F:\\SteamLibrary\\steamapps\\common\\DIRT 5\\DIRT5.exe");if(!std::filesystem::is_regular_file(gamePath)){auto local=std::filesystem::path(folder).parent_path()/L"DIRT5.exe";if(std::filesystem::is_regular_file(local)){gamePath=local.wstring();WritePrivateProfileStringW(L"Game",L"Exe",gamePath.c_str(),settings.c_str());}}
 backgroundBrush=CreateSolidBrush(background);fieldBrush=CreateSolidBrush(field);
 WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=instance;wc.lpszClassName=L"DIRT5OpenXRModLauncher";wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_LAUNCHER));wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=backgroundBrush;RegisterClassW(&wc);
 window=CreateWindowExW(0,wc.lpszClassName,L"DIRT 5 • OpenXR Mod 0.2",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,820,820,nullptr,nullptr,instance,nullptr);
 SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_LAUNCHER),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED)));
 font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 headingFont=CreateFontW(-32,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 supportFont=CreateFontW(-18,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 storeFont=CreateFontW(-28,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
 auto heading=control(L"STATIC",L"DIRT 5",0,24,18,360,42);SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(headingFont),TRUE);
 control(L"STATIC",L"OPENXR  /  VR MOD",0,590,31,188,26);
 control(L"STATIC",L"Menus: cinema screen. Cockpit racing: VR. Monitor: mono.",0,24,68,760,27);
 control(L"STATIC",L"OpenXR",0,24,115,100,25);runtimeBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,125,108,642,140,ID_RUNTIME);
 for(auto label:{L"SteamVR OpenXR",L"System OpenXR runtime",L"Custom OpenXR runtime JSON"})SendMessageW(runtimeBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
 auto mode=setting(L"OpenXR",L"Runtime",L"system");int selected=mode==L"system"?1:mode==L"custom"?2:0;SendMessageW(runtimeBox,CB_SETCURSEL,selected,0);
 jsonBox=control(L"EDIT",setting(L"OpenXR",L"RuntimeJson",L"").c_str(),WS_BORDER|ES_AUTOHSCROLL,24,155,642,30,ID_JSON);EnableWindow(jsonBox,selected!=1);if(selected==0)SetWindowTextW(jsonBox,steamRuntimeJson().c_str());control(L"BUTTON",L"Browse",WS_TABSTOP,678,155,88,30,ID_BROWSE);
 control(L"STATIC",L"Headset resolution per eye",0,24,209,740,26);
 control(L"STATIC",L"Width",0,24,248,85,25);widthBox=control(L"EDIT",setting(L"OpenXR",L"EyeWidth",L"2000").c_str(),WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,110,243,130,32,ID_WIDTH);
 control(L"STATIC",L"Height",0,268,248,85,25);heightBox=control(L"EDIT",setting(L"OpenXR",L"EyeHeight",L"2000").c_str(),WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,359,243,130,32,ID_HEIGHT);control(L"BUTTON",L"Save",WS_TABSTOP,517,243,249,32,ID_SAVE);
 control(L"STATIC",L"Sets the VR scene resolution. Set monitor resolution in the game.",0,24,292,750,28);
 control(L"STATIC",L"HUD distance, meters",0,24,341,210,25);hudDistanceBox=control(L"EDIT",setting(L"HUD",L"DistanceMeters",L"3").c_str(),WS_BORDER|ES_AUTOHSCROLL,235,335,100,32,ID_HUD_DISTANCE);
 control(L"STATIC",L"Higher resolution increases GPU load.",0,24,388,750,25);
 startButton=control(L"BUTTON",L"Launch / Start VR",WS_TABSTOP,24,431,742,47,ID_START);
 control(L"BUTTON",L"Recenter",WS_TABSTOP,24,495,230,38,ID_CENTER);control(L"BUTTON",L"Stop VR",WS_TABSTOP,277,495,232,38,ID_STOP);control(L"BUTTON",L"Open Mod Folder",WS_TABSTOP,533,495,233,38,ID_FOLDER);
 statusBox=control(L"EDIT",L"Connect your headset and start SteamVR, then click Launch / Start VR.\r\nYou can attach to a running game. Recenter with either Ctrl key.",WS_BORDER|ES_MULTILINE|ES_READONLY,24,555,742,48);
 control(L"STATIC",L"",SS_OWNERDRAW,24,625,742,1,ID_SUPPORT_LINE);
 control(L"STATIC",L"FOR SUPPORT DEVELOPER",SS_OWNERDRAW,24,639,742,25,ID_SUPPORT_TITLE);
 control(L"STATIC",L"Buy CYBRID on Steam or Quest Store",SS_OWNERDRAW,24,663,742,27,ID_SUPPORT_DESCRIPTION);
 control(L"BUTTON",L"STEAM",WS_TABSTOP,120,705,254,42,ID_SUPPORT_STEAM);
 control(L"BUTTON",L"QUEST",WS_TABSTOP,422,705,254,42,ID_SUPPORT_QUEST);
 ShowWindow(window,show);UpdateWindow(window);MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}return 0;
}
