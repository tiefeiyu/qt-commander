// DI injector logic tests — all injector operations via MockProcessOps.
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include "os_ops.h"
#include "injector.h"
namespace fs = std::filesystem;
static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

// --- injectLibrary ---
void t1() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=true; o.thread_exit_code=0x7FFE0000; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(r.ok,"inject ok"); CHECK(o.calls.size()==4,"4 calls"); }
void t2() { MockProcessOps o; o.open_process_result=false; o.last_error_str="denied"; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"open fails"); }
void t3() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=false; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"alloc fails"); }
void t4() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=false; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"write fails"); }
void t5() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=false; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"thread fails"); }
void t6() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=false; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"timeout"); }
void t7() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=true; o.thread_exit_code=0; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"null base"); }
void t8() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.get_load_library_addr_result=false; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"getLL fails"); }
void t9() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=true; o.get_thread_exit_code_result=false; auto r=injectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"getExitCode fails"); }

// --- isQtProcess ---
void ta() { MockProcessOps o; o.open_process_result=true; o.module_names={"k32.dll","Qt5Core.dll","u32.dll"}; CHECK(isQtProcess(o,1234),"Qt5 ok"); }
void tb() { MockProcessOps o; o.open_process_result=true; o.module_names={"n.dll","Qt6Core.dll"}; CHECK(isQtProcess(o,5678),"Qt6 ok"); }
void tc() { MockProcessOps o; o.open_process_result=true; o.module_names={"k32.dll","u32.dll"}; CHECK(!isQtProcess(o,9999),"no Qt"); }
void td() { MockProcessOps o; o.open_process_result=false; CHECK(!isQtProcess(o,1234),"open fails"); }
void te() { MockProcessOps o; o.open_process_result=true; o.module_names={}; CHECK(!isQtProcess(o,1234),"empty mods"); }
void tf() { MockProcessOps o; o.open_process_result=true; o.enum_modules_result=false; CHECK(!isQtProcess(o,1234),"enum fails"); }

// --- generateToken ---
void tg() { MockProcessOps o; o.random_result=true; o.random_bytes.assign(32,0xFF); std::string t=generateToken(o); CHECK(t.size()==64,"len 64"); CHECK(t==std::string(64,'f'),"all f"); }
void th() { MockProcessOps o; o.random_result=true; o.random_bytes.assign(32,0xAB); std::string t=generateToken(o); bool ok=true; for(char c:t)if(c!='a'&&c!='b')ok=false; CHECK(ok,"only a/b"); }
void ti() { MockProcessOps o; o.random_result=false; bool threw=false; try{generateToken(o);}catch(const std::runtime_error&){threw=true;} CHECK(threw,"CSPRNG fail throws"); }

// --- ejectLibrary ---
void tj() { MockProcessOps o; o.open_process_result=true; o.module_names={"k32.dll","libqt-commander.dll"}; o.create_thread_result=true; o.wait_result=true; auto r=ejectLibrary(o,1234,fs::path("C:\\libqt-commander.dll")); CHECK(r.ok||r.error.find("GetProcAddress")!=std::string::npos,"eject ok/GetProcAddr err"); }
void tk() { MockProcessOps o; o.open_process_result=false; auto r=ejectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"open fails"); }
void tl() { MockProcessOps o; o.open_process_result=true; o.module_names={"k32.dll"}; auto r=ejectLibrary(o,1234,fs::path("C:\\libqt-commander.dll")); CHECK(!r.ok,"dll not found"); }
void tm() { MockProcessOps o; o.open_process_result=true; o.enum_modules_result=false; auto r=ejectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"enum fails"); }
void tn() { MockProcessOps o; o.open_process_result=true; o.module_names={"l.dll"}; o.get_module_handle_result=false; auto r=ejectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"getMod fails"); }
void to() { MockProcessOps o; o.open_process_result=true; o.module_names={"l.dll"}; o.get_module_handle_result=true; o.get_proc_address_result=false; auto r=ejectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"getProc fails"); }
void tp() { MockProcessOps o; o.open_process_result=true; o.module_names={"l.dll"}; o.get_module_handle_result=true; o.get_proc_address_result=true; o.create_thread_result=false; auto r=ejectLibrary(o,1234,fs::path("C:\\l.dll")); CHECK(!r.ok,"createThread fails"); }

// --- performInitHandshake ---
void tq() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=true; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; o.module_names={"libqt-commander.dll"}; auto pf=fs::temp_directory_path()/"tq.txt"; {std::ofstream f(pf);f<<"23456\naaaaBBBBccccDDDDeeeeFFFFggggHHHHiiiiJJJJkkkkLLLLmmmmNNNN\n";} uint16_t p=performInitHandshake(o,1234,fs::path("C:\\libqt-commander.dll"),"w","s","aaaaBBBBccccDDDDeeeeFFFFggggHHHHiiiiJJJJkkkkLLLLmmmmNNNN",pf); CHECK(p==23456,"init ok"); fs::remove(pf); }
void tr() { MockProcessOps o; o.open_process_result=false; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; CHECK(performInitHandshake(o,1234,fs::path("C:\\l.dll"),"w","s","t",fs::temp_directory_path()/"tr.txt")==0,"open fails"); }
void ts() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=true; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; o.module_names={"libqt-commander.dll"}; auto pf=fs::temp_directory_path()/"ts.txt"; {std::ofstream f(pf);f<<"1\nwrong\n";} CHECK(performInitHandshake(o,1234,fs::path("C:\\libqt-commander.dll"),"w","s","aaaaBBBBccccDDDDeeeeFFFFggggHHHHiiiiJJJJkkkkLLLLmmmmNNNN",pf)==0,"token mismatch"); fs::remove(pf); }
void tt() { MockProcessOps o; o.read_file_result=false; CHECK(performInitHandshake(o,1234,fs::path("C:\\l.dll"),"w","s","t",fs::temp_directory_path()/"tt.txt")==0,"read fails"); }
void tu() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.create_thread_result=true; o.wait_result=false; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; o.module_names={"libqt-commander.dll"}; CHECK(performInitHandshake(o,1234,fs::path("C:\\libqt-commander.dll"),"w","s","t",fs::temp_directory_path()/"tu.txt")==0,"timeout"); }
void tv() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; o.enum_modules_result=false; CHECK(performInitHandshake(o,1234,fs::path("C:\\l.dll"),"w","s","t",fs::temp_directory_path()/"tv.txt")==0,"enum fails"); }
void tw() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; o.module_names={"other.dll"}; CHECK(performInitHandshake(o,1234,fs::path("C:\\l.dll"),"w","s","t",fs::temp_directory_path()/"tw.txt")==0,"dll not found"); }
void tx() { MockProcessOps o; o.open_process_result=true; o.alloc_mem_result=true; o.write_mem_result=true; o.read_file_result=true; o.file_bytes={0x4D,0x5A}; o.module_names={"l.dll"}; o.create_thread_result=false; CHECK(performInitHandshake(o,1234,fs::path("C:\\l.dll"),"w","s","t",fs::temp_directory_path()/"tx.txt")==0,"createThread fails"); }

int main() {
    t1();t2();t3();t4();t5();t6();t7();t8();t9();
    ta();tb();tc();td();te();tf();
    tg();th();ti();
    tj();tk();tl();tm();tn();to();tp();
    tq();tr();ts();tt();tu();tv();tw();tx();
    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
