#pragma once
#include <windows.h>
#include <array>
#include <cstring>

// Native RVA 192a90 takes seven arguments. Its sixth is a 16-byte policy.
// When policy[3] is zero it multiplies projection row 2 by (0,0,1,1),
// treating both optical-centre terms as desktop temporal jitter. That also
// erases OpenXR's asymmetric frustum. Preserve them only for the active VR
// view; pass a private policy copy and never alter the caller's structure.
using DerivedCameraFn=void(*)(void*,const void*,const void*,const void*,
                              const void*,const void*,const void*);
inline bool copiedOffAxisPolicy(const void* policy,std::array<BYTE,16>& copy) {
 if(!policy)return false;
 SIZE_T got=0;
 if(!ReadProcessMemory(GetCurrentProcess(),policy,copy.data(),copy.size(),&got)||got!=copy.size())return false;
 UINT flag=0;memcpy(&flag,copy.data()+12,4);
 if(flag)return false;
 flag=1;memcpy(copy.data()+12,&flag,4);return true;
}
