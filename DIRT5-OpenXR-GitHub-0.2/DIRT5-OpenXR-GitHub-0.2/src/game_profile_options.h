#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>

// Call only while the game is closed, so its profile writer cannot race us.
inline void disableDriverNames(const std::filesystem::path& profile){
 if(!std::filesystem::exists(profile))return;
 if(std::filesystem::file_size(profile)>4*1024*1024)throw std::runtime_error("Profile too large");
 std::ifstream input(profile,std::ios::binary);if(!input)throw std::runtime_error("Profile unavailable");
 std::string bytes((std::istreambuf_iterator<char>(input)),{});input.close();
 if(bytes.size()<18||bytes.compare(0,8,"lfrPrylP")!=0||bytes.back()!=0||bytes.find("\"objectInstances\"")==std::string::npos)throw std::runtime_error("Unknown profile format");
 const std::string key="\"DriverNamesEnabled\"";size_t value=std::string::npos;
 for(size_t at=0;(at=bytes.find(key,at))!=std::string::npos;at+=key.size()){
  size_t p=bytes.find_first_not_of(" \t\r\n",at+key.size());if(p==std::string::npos||bytes[p]!=':')continue;
  p=bytes.find_first_not_of(" \t\r\n",p+1);if(p==std::string::npos)continue;
  if(value!=std::string::npos)throw std::runtime_error("Ambiguous driver names setting");value=p;
 }
 if(value==std::string::npos)throw std::runtime_error("Driver names setting missing");
 if(bytes.compare(value,5,"false")==0)return;
 if(bytes.compare(value,4,"true")!=0)throw std::runtime_error("Driver names setting is not boolean");
 bytes.replace(value,4,"false");
 auto backup=profile.parent_path()/L"settings.before_dirt5vr_names.json";
 if(!CopyFileW(profile.c_str(),backup.c_str(),TRUE)&&GetLastError()!=ERROR_FILE_EXISTS)throw std::runtime_error("Profile backup failed");
 auto temporary=profile.parent_path()/(L"settings.dirt5vr."+std::to_wstring(GetCurrentProcessId())+L".tmp");
 {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output.write(bytes.data(),bytes.size());output.flush();if(!output)throw std::runtime_error("Profile write failed");}
 if(!ReplaceFileW(profile.c_str(),temporary.c_str(),nullptr,0,nullptr,nullptr))throw std::runtime_error("Profile replace failed");
}
