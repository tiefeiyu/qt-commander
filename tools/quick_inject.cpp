// Quick inject + snapshot tool for a specific PID
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <fstream>
#include <random>
#include <algorithm>
#include <chrono>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "common/socket_utils.h"
#include "common/framing.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

#define IP_VERSION 1
#define IP_MAX_PATH 256
#define IP_TOKEN_LEN 64
#define IP_TOTAL_SIZE 1024
struct InitParams { uint32_t version; uint32_t total_size; char workspace_path[256]; char session_id[13]; char token[65]; char port_file_path[256]; uint8_t reserved[426]; };

static fs::path findFile(const std::string& rel) { fs::path d=fs::absolute(fs::current_path()); for(int i=0;i<6;i++){if(fs::exists(d/rel))return d/rel;if(!d.has_parent_path())break;d=d.parent_path();} return{}; }
static fs::path findFileInTree(const std::string& filename) { fs::path d=fs::absolute(fs::current_path()); for(int i=0;i<6;i++){std::error_code ec;auto it=fs::recursive_directory_iterator(d,fs::directory_options::skip_permission_denied,ec);if(!ec){for(auto& e:it){if(e.path().filename()==filename)return e.path();}}if(!d.has_parent_path())break;d=d.parent_path();}return{}; }
static std::string randHex(int n){static std::mt19937 g(std::random_device{}());static std::uniform_int_distribution<> d(0,15);const char* h="0123456789abcdef";std::string s;for(int i=0;i<n*2;i++)s+=h[d(g)];return s;}
static bool sendFrame(socket_t fd,const std::string& d){auto f=frame_encode((const uint8_t*)d.data(),d.size());return tcp_send_all(fd,f.data(),f.size());}
static std::string recvFrame(socket_t fd,int to=30000){tcp_set_recv_timeout(fd,to);FrameDecoder dec;uint8_t b;for(int i=0;i<500000;i++){if(!tcp_recv_all(fd,&b,1))return{};std::vector<uint8_t> o;auto r=dec.feed(&b,1,o);if(r==FrameResult::Error)return{};if(r==FrameResult::Complete)return std::string(o.begin(),o.end());}return{};}
static std::string rpc(const std::string& m,const std::string& p,int id){return"{\"jsonrpc\":\"2.0\",\"method\":\""+m+"\",\"params\":"+p+",\"id\":"+std::to_string(id)+"}";}

static uint64_t resolveExport(const fs::path& dll,const std::string& name,uint64_t base){HANDLE hf=CreateFileW(dll.wstring().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(hf==INVALID_HANDLE_VALUE)return 0;HANDLE hm=CreateFileMappingW(hf,nullptr,PAGE_READONLY,0,0,nullptr);if(!hm){CloseHandle(hf);return 0;}auto* b=(const uint8_t*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);auto* dos=(const IMAGE_DOS_HEADER*)b;auto* nt=(const IMAGE_NT_HEADERS64*)(b+dos->e_lfanew);auto& ed=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];auto* sec=IMAGE_FIRST_SECTION(nt);auto r2o=[&](DWORD rva)->const uint8_t*{for(int i=0;i<nt->FileHeader.NumberOfSections;i++)if(rva>=sec[i].VirtualAddress&&rva<sec[i].VirtualAddress+sec[i].Misc.VirtualSize)return b+sec[i].PointerToRawData+(rva-sec[i].VirtualAddress);return nullptr;};auto* exp=(const IMAGE_EXPORT_DIRECTORY*)r2o(ed.VirtualAddress);auto* names=(const DWORD*)r2o(exp->AddressOfNames);auto* ords=(const WORD*)r2o(exp->AddressOfNameOrdinals);auto* funcs=(const DWORD*)r2o(exp->AddressOfFunctions);uint64_t rva=0;for(DWORD j=0;j<exp->NumberOfNames;j++){const char* n=(const char*)r2o(names[j]);if(n&&name==n){rva=funcs[ords[j]];break;}}UnmapViewOfFile(b);CloseHandle(hm);CloseHandle(hf);return rva?base+rva:0;}

int main(int argc, char* argv[]) {
    if (argc < 2) { std::cerr << "Usage: quick_inject <PID>\n"; return 1; }
    int pid = std::stoi(argv[1]);
    std::cout << "=== Injecting into PID " << pid << " ===\n\n";

    // DLL path: 2nd argument, or search from current directory
    fs::path dll;
    if (argc >= 3) {
        dll = argv[2];
    } else {
        for (fs::path d = fs::absolute(fs::current_path()); d.has_parent_path(); d = d.parent_path()) {
            fs::path candidate = d / "src" / "library" / "Debug" / "libqt-commander.dll";
            if (fs::exists(candidate)) { dll = candidate; break; }
        }
    }
    if (dll.empty()) { std::cerr << "DLL not found. Usage: quick_inject <PID> [dll_path]\n"; return 1; }
    std::cout << "DLL: " << dll.string() << "\n";

    // 1. Inject DLL
    HANDLE hp=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
    if(!hp){std::cerr<<"OpenProcess failed: "<<GetLastError()<<"\n";return 1;}
    std::wstring wd=dll.wstring();size_t sz=(wd.size()+1)*sizeof(wchar_t);
    void* rp=VirtualAllocEx(hp,nullptr,sz,MEM_COMMIT,PAGE_READWRITE);
    WriteProcessMemory(hp,rp,wd.c_str(),sz,nullptr);
    auto pLL=(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HANDLE ht=CreateRemoteThread(hp,nullptr,0,pLL,rp,0,nullptr);
    WaitForSingleObject(ht,10000);CloseHandle(ht);VirtualFreeEx(hp,rp,0,MEM_RELEASE);
    std::this_thread::sleep_for(200ms);

    // Get 64-bit DLL base
    HMODULE mods[2048];DWORD needed=0;uint64_t base=0;
    if(EnumProcessModules(hp,mods,sizeof(mods),&needed)){int c=needed/sizeof(HMODULE);
    for(int i=0;i<c;i++){wchar_t n[MAX_PATH];if(GetModuleBaseNameW(hp,mods[i],n,MAX_PATH)&&_wcsicmp(n,L"libqt-commander.dll")==0){base=(uint64_t)mods[i];break;}}}
    if(!base){std::cerr<<"DLL not loaded\n";CloseHandle(hp);return 1;}
    std::cout << "DLL base: 0x" << std::hex << base << std::dec << "\n";

    // 2. Init
    uint64_t initAddr=resolveExport(dll,"qt_commander_init",base);
    if(!initAddr){std::cerr<<"Export not found\n";CloseHandle(hp);return 1;}

    auto ws=fs::temp_directory_path()/"qt-commander-quick";
    fs::create_directories(ws/"sessions"/"quick");fs::create_directories(ws/"logs");
    std::string sid="quick000000",tok=randHex(32);auto pf=ws/"sessions"/"quick"/"port.txt";
    std::string wss=ws.string(),pfs=pf.string();
    std::replace(wss.begin(),wss.end(),'\\','/');std::replace(pfs.begin(),pfs.end(),'\\','/');

    InitParams ip={};ip.version=IP_VERSION;ip.total_size=IP_TOTAL_SIZE;
    strncpy_s(ip.workspace_path,sizeof(ip.workspace_path),wss.c_str(),_TRUNCATE);
    strncpy_s(ip.session_id,sizeof(ip.session_id),sid.c_str(),_TRUNCATE);
    strncpy_s(ip.token,sizeof(ip.token),tok.c_str(),_TRUNCATE);
    strncpy_s(ip.port_file_path,sizeof(ip.port_file_path),pfs.c_str(),_TRUNCATE);

    void* rp2=VirtualAllocEx(hp,nullptr,sizeof(InitParams),MEM_COMMIT,PAGE_READWRITE);
    WriteProcessMemory(hp,rp2,&ip,sizeof(InitParams),nullptr);
    HANDLE ht2=CreateRemoteThread(hp,nullptr,0,(LPTHREAD_START_ROUTINE)initAddr,rp2,0,nullptr);
    WaitForSingleObject(ht2,10000);DWORD ec=99;GetExitCodeThread(ht2,&ec);CloseHandle(ht2);
    VirtualFreeEx(hp,rp2,0,MEM_RELEASE);CloseHandle(hp);
    if(ec!=0){std::cerr<<"Init failed: "<<ec<<"\n";fs::remove_all(ws);return 1;}
    std::cout << "Init OK\n";

    // 3. Read port file
    socket_init();
    uint16_t port=0;std::string ftok;
    for(int i=0;i<40;i++){std::this_thread::sleep_for(std::chrono::milliseconds(100+i*50));std::ifstream pfs2(pf);if(pfs2.is_open()){std::string l;if(std::getline(pfs2,l)){port=(uint16_t)std::stoi(l);if(std::getline(pfs2,l)){ftok=l;if(port>0&&!ftok.empty())break;}}}}
    std::cout << "Port: " << port << " Token: " << ftok.substr(0,16) << "...\n";

    // 4. Connect + Auth
    socket_t sock=tcp_connect_loopback(port);
    if(sock==INVALID_SOCK){std::cerr<<"Connect failed\n";fs::remove_all(ws);return 1;}
    sendFrame(sock,rpc("qt.authenticate","{\"token\":\""+tok+"\"}",0));
    std::string ar=recvFrame(sock,5000);
    if(ar.find("\"result\"")==std::string::npos){std::cerr<<"Auth failed: "<<ar<<"\n";tcp_close(sock);fs::remove_all(ws);return 1;}
    std::cout << "Auth OK\n";

    // 5. Take multiple snapshots with delays
    for (int snap = 1; snap <= 3; snap++) {
        if (snap > 1) { std::cout << "\nWaiting 3s for UI to initialize...\n"; std::this_thread::sleep_for(3s); }
        std::cout << "\n--- Snapshot #" << snap << " ---\n";
        sendFrame(sock,rpc("qt.snapshot","{\"detail\":\"core\"}",snap));
        std::string sr=recvFrame(sock,30000);
        if (sr.empty()) { std::cout << "Timeout!\n"; break; }
        std::cout << sr.substr(0, 4000) << "\n";
    }

    // 6. Shutdown
    sendFrame(sock,rpc("qt.shutdown","{}",0));std::this_thread::sleep_for(500ms);
    tcp_close(sock);fs::remove_all(ws);socket_cleanup();
    std::cout << "\nDone.\n";
    return 0;
}
