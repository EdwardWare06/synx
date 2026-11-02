// Native Windows debugger acceptance test. Uses the OS debug API and DbgHelp;
// no Visual Studio, DIA registration, LLVM, or symbol-server access is needed.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void fail(const char* operation) {
  std::fprintf(stderr, "%s: Windows error %lu\n", operation, GetLastError());
  std::fflush(stdout);
  ExitProcess(1);
}
#include "unwind.h"
struct Lines { DWORD64 callee=0, caller=0; };
static BOOL CALLBACK line(PSRCCODEINFOW value, PVOID context) {
  auto& result=*static_cast<Lines*>(context);
  if(std::wcsstr(value->FileName,L"calc.snx") && value->LineNumber==3)result.callee=value->Address;
  if(std::wcsstr(value->FileName,L"main.snx") && value->LineNumber==4)result.caller=value->Address;
  return TRUE;
}
struct Breakpoint { DWORD64 address=0; BYTE original=0; bool active=false; };
static void set(HANDLE process,Breakpoint& b,DWORD64 address) {
  b.address=address; SIZE_T count=0;
  if(!ReadProcessMemory(process,(void*)address,&b.original,1,&count)||count!=1)fail("read breakpoint");
  BYTE trap=0xcc;
  if(!WriteProcessMemory(process,(void*)address,&trap,1,&count)||count!=1)fail("write breakpoint");
  FlushInstructionCache(process,(void*)address,1);b.active=true;
}
static void clear(HANDLE process,Breakpoint& b) {
  SIZE_T count=0;
  if(!WriteProcessMemory(process,(void*)b.address,&b.original,1,&count)||count!=1)fail("restore breakpoint");
  FlushInstructionCache(process,(void*)b.address,1);b.active=false;
}
static IMAGEHLP_LINEW64 location(HANDLE process,DWORD64 address) {
  IMAGEHLP_LINEW64 result{};result.SizeOfStruct=sizeof(result);DWORD displacement=0;
  if(!SymGetLineFromAddrW64(process,address,&displacement,&result))fail("source line");
  return result;
}
static std::string symbol(HANDLE process,DWORD64 address) {
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
  auto value=reinterpret_cast<SYMBOL_INFO*>(storage);value->SizeOfStruct=sizeof(SYMBOL_INFO);value->MaxNameLen=MAX_SYM_NAME;
  DWORD64 displacement=0;
  return SymFromAddr(process,address,&displacement,value)?std::string(value->Name):"?";
}
int wmain(int argc,wchar_t** argv) {
  if(argc!=2&&argc!=3)return 2;bool attach=argc==3&&!std::wcscmp(argv[2],L"--attach");
  try {auto count=virtual_unwind(argv[1]);if(count)std::printf("Windows virtual unwind: %u instruction boundaries\n",count);}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);PROCESS_INFORMATION process{};
  SIZE_T attribute_size=0;InitializeProcThreadAttributeList(nullptr,1,0,&attribute_size);std::vector<BYTE> attributes(attribute_size);
  startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size))fail("ASLR attributes");
  DWORD64 policy=PROCESS_CREATION_MITIGATION_POLICY_FORCE_RELOCATE_IMAGES_ALWAYS_ON_REQ_RELOCS|PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON|PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON;
  if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,&policy,sizeof(policy),nullptr,nullptr))fail("mandatory ASLR policy");
  HANDLE input_read=nullptr,input_write=nullptr,output_read=nullptr,output_write=nullptr;
  if(attach){SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};if(!CreatePipe(&input_read,&input_write,&security,0)||!CreatePipe(&output_read,&output_write,&security,0))fail("attach pipes");
    SetHandleInformation(input_write,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output_read,HANDLE_FLAG_INHERIT,0);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=input_read;startup.StartupInfo.hStdOutput=output_write;startup.StartupInfo.hStdError=output_write;
  }
  std::wstring command=L"\""+std::wstring(argv[1])+L"\"";
  if(!CreateProcessW(argv[1],command.data(),nullptr,nullptr,attach,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|(attach?0:DEBUG_ONLY_THIS_PROCESS),nullptr,nullptr,&startup.StartupInfo,&process))fail("start debuggee");
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  if(attach){CloseHandle(input_read);CloseHandle(output_write);std::string ready;char c;DWORD n=0;while(ReadFile(output_read,&c,1,&n,nullptr)&&n&&c!='\n')ready+=c;if(ready!="READY"&&ready!="READY\r")fail("attach startup marker");if(!DebugActiveProcess(process.dwProcessId))fail("attach running process");}
  SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_NO_PROMPTS|SYMOPT_FAIL_CRITICAL_ERRORS);
  if(!SymInitializeW(process.hProcess,L"",FALSE))fail("initialize symbols");
  Breakpoint callee,caller;bool breakpoint=false,stepped=false,returned=false,stack=false;unsigned steps=0;
  DWORD64 module=0;DWORD exit=1;
  for(;;) {
    DEBUG_EVENT event{};if(!WaitForDebugEvent(&event,30000))fail("wait debug event");DWORD status=DBG_CONTINUE;
    if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
      module=(DWORD64)event.u.CreateProcessInfo.lpBaseOfImage;
      std::printf("ASLR module=%llx preferred=%llx\n",module,preferred_base);
      if(module==preferred_base)fail("mandatory ASLR rebases image");
      if(!SymLoadModuleExW(process.hProcess,event.u.CreateProcessInfo.hFile,argv[1],nullptr,module,0,nullptr,0))fail("load executable PDB");
      if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
      Lines lines;if(!SymEnumLinesW(process.hProcess,module,nullptr,nullptr,line,&lines)||!lines.callee||!lines.caller)fail("find source breakpoints");
      set(process.hProcess,callee,lines.callee);set(process.hProcess,caller,lines.caller);
      if(attach){DWORD n=0;if(!WriteFile(input_write,"\n",1,&n,nullptr)||n!=1)fail("release attached process");CloseHandle(input_write);input_write=nullptr;}
    } else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT) {
      if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);
    } else if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
      auto exception=event.u.Exception.ExceptionRecord;HANDLE thread=OpenThread(THREAD_ALL_ACCESS,FALSE,event.dwThreadId);if(!thread)fail("open debug thread");
      CONTEXT context{};context.ContextFlags=CONTEXT_FULL;if(!GetThreadContext(thread,&context))fail("read context");
      if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && callee.active && (DWORD64)exception.ExceptionAddress==callee.address) {
        clear(process.hProcess,callee);context.Rip=callee.address;
        auto source=location(process.hProcess,context.Rip);
        if(source.LineNumber!=3||!std::wcsstr(source.FileName,L"calc.snx"))fail("callee source breakpoint");
        auto name=symbol(process.hProcess,context.Rip);if(name.find("adjust")==std::string::npos)fail("callee function symbol");
        CONTEXT walk=context;STACKFRAME64 frame{};frame.AddrPC={walk.Rip,0,AddrModeFlat};frame.AddrFrame={walk.Rbp,0,AddrModeFlat};frame.AddrStack={walk.Rsp,0,AddrModeFlat};
        std::vector<std::string> frames;
        for(int i=0;i<16;++i) { if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process.hProcess,thread,&frame,&walk,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)||!frame.AddrPC.Offset)break;frames.push_back(symbol(process.hProcess,frame.AddrPC.Offset)); }
        bool main=false,bridge=false;for(auto& value:frames){std::printf("frame %s\n",value.c_str());if(value.find("main")!=std::string::npos)main=true;if(value=="synx_entry")bridge=true;}
        if(!main||!bridge)fail("stack through caller and startup bridge");stack=true;breakpoint=true;context.EFlags|=0x100;
      } else if(exception.ExceptionCode==EXCEPTION_SINGLE_STEP && breakpoint && !stepped) {
        IMAGEHLP_LINEW64 source{};source.SizeOfStruct=sizeof(source);DWORD displacement=0;
        if(SymGetLineFromAddrW64(process.hProcess,context.Rip,&displacement,&source) && source.LineNumber==4 && std::wcsstr(source.FileName,L"calc.snx")){stepped=true;context.EFlags&=~0x100u;}
        if(!stepped)context.EFlags|=0x100;
        if(++steps>20000)fail("source step limit");
      } else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && caller.active && (DWORD64)exception.ExceptionAddress==caller.address) {
        clear(process.hProcess,caller);context.Rip=caller.address;auto source=location(process.hProcess,context.Rip);
        if(!stepped||source.LineNumber!=4||!std::wcsstr(source.FileName,L"main.snx")){std::printf("return stepped=%u steps=%u line=%lu file=%ls\n",unsigned(stepped),steps,source.LineNumber,source.FileName);fail("return to caller source");}returned=true;
      } else if(exception.ExceptionCode!=EXCEPTION_BREAKPOINT)status=DBG_EXCEPTION_NOT_HANDLED;
      if(status==DBG_CONTINUE && !SetThreadContext(thread,&context))fail("write context");CloseHandle(thread);
    } else if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)exit=event.u.ExitProcess.dwExitCode;
    if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,status))fail("continue debug event");
    if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)break;
  }
  SymCleanup(process.hProcess);CloseHandle(process.hThread);CloseHandle(process.hProcess);
  if(output_read)CloseHandle(output_read);
  if(exit||!breakpoint||!stepped||!returned||!stack)return 1;
  if(attach)std::puts("SYNX_WINDOWS_ATTACH_OK");std::puts("SYNX_WINDOWS_DEBUG_OK");return 0;
}
