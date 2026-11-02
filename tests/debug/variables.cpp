// Inspect source variables through the Windows debug API and DbgHelp. Resolve
// native locations and type members, then read the stopped process directly.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const char* what) { if(!ok)throw std::runtime_error(std::string(what)+" (Windows "+std::to_string(GetLastError())+")"); }
struct Var { DWORD type, flags, reg, size; DWORD64 address; };
struct Variables { HANDLE process; std::map<std::string,Var> values; };
static BOOL CALLBACK variable(PSYMBOL_INFO s, ULONG, void* p) {
  if(s->Flags&SYMFLAG_LOCAL||s->Flags&SYMFLAG_PARAMETER){auto& vars=*static_cast<Variables*>(p);ULONG64 size=0;require(SymGetTypeInfo(vars.process,s->ModBase,s->TypeIndex,TI_GET_LENGTH,&size),"local type size");
    vars.values[s->Name]={s->TypeIndex,s->Flags,s->Register,(DWORD)size,s->Address};}
  return TRUE;
}
static DWORD64 reg(const CONTEXT& c,DWORD n) {
  DWORD64 r[]={c.Rax,c.Rcx,c.Rdx,c.Rbx,c.Rsp,c.Rbp,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15};
  require(n>=328&&n<=343,"AMD64 register number");return r[n-328];
}
static std::map<std::string,Var> locals(HANDLE p,const CONTEXT& c) {
  IMAGEHLP_STACK_FRAME f{};f.InstructionOffset=c.Rip;f.FrameOffset=c.Rbp;f.StackOffset=c.Rsp;SetLastError(ERROR_SUCCESS);
  require(SymSetContext(p,&f,nullptr)||GetLastError()==ERROR_SUCCESS,"set local scope");
  Variables result{p,{}};require(SymEnumSymbols(p,0,nullptr,variable,&result),"enumerate source locals");return result.values;
}
static bool available(const Var& v) {return (v.flags&(SYMFLAG_REGISTER|SYMFLAG_REGREL|SYMFLAG_FRAMEREL|SYMFLAG_VALUEPRESENT))!=0;}
static DWORD64 address(const Var& v,const CONTEXT& c) {
  if(v.flags&SYMFLAG_REGREL)return reg(c,v.reg)+v.address;
  if(v.flags&SYMFLAG_FRAMEREL)return c.Rbp+v.address;
  throw std::runtime_error("variable has no memory location");
}
static DWORD64 memory(HANDLE p,DWORD64 at,ULONG size) {DWORD64 v=0;SIZE_T n=0;require(size<=8&&ReadProcessMemory(p,(void*)at,&v,size,&n)&&n==size,"read variable memory");return v;}
static DWORD64 scalar(HANDLE p,const Var& v,const CONTEXT& c) {
  require(available(v),"variable is available");if(v.flags&SYMFLAG_REGISTER)return reg(c,v.reg);
  if(v.flags&SYMFLAG_VALUEPRESENT)return v.address;return memory(p,address(v,c),v.size);
}
static Var member(HANDLE p,DWORD64 module,const Var& parent,const CONTEXT& c,const wchar_t* name) {
  DWORD count=0;require(SymGetTypeInfo(p,module,parent.type,TI_GET_CHILDRENCOUNT,&count),"type field count");
  std::vector<BYTE> storage(sizeof(TI_FINDCHILDREN_PARAMS)+count*sizeof(ULONG));auto children=(TI_FINDCHILDREN_PARAMS*)storage.data();children->Count=count;
  require(SymGetTypeInfo(p,module,parent.type,TI_FINDCHILDREN,children),"type field indices");
  for(DWORD i=0;i<count;++i){WCHAR* field=nullptr;require(SymGetTypeInfo(p,module,children->ChildId[i],TI_GET_SYMNAME,&field),"field name");bool match=!wcscmp(field,name);LocalFree(field);if(!match)continue;
    DWORD offset=0,type=0;ULONG64 size=0;require(SymGetTypeInfo(p,module,children->ChildId[i],TI_GET_OFFSET,&offset),"field offset");require(SymGetTypeInfo(p,module,children->ChildId[i],TI_GET_TYPEID,&type),"field type");require(SymGetTypeInfo(p,module,type,TI_GET_LENGTH,&size),"field size");
    return {type,SYMFLAG_REGREL,333,(DWORD)size,address(parent,c)+offset-c.Rbp};
  }throw std::runtime_error("missing type field");
}
struct Point { DWORD line; DWORD64 address=0; BYTE original=0; bool active=false; };
struct Points { std::map<std::string,Point> points; };
static BOOL CALLBACK line(PSRCCODEINFOW s,void* p) {if(!wcsstr(s->FileName,L"variables.snx"))return TRUE;for(auto& [name,b]:static_cast<Points*>(p)->points)if(b.line==s->LineNumber&&(!b.address||s->Address<b.address))b.address=s->Address;return TRUE;}
int wmain(int argc,wchar_t** argv) {
 try {
  if(argc!=3)return 2;std::ifstream source{std::wstring(argv[2])};require(bool(source),"open source fixture");Points points;std::string text;DWORD number=0;
  while(std::getline(source,text)){++number;for(auto marker:{"VALUES","SHADOW","OUTER","INNER","RESULT"})if(text.find(std::string("// ")+marker)!=std::string::npos)points.points[marker]={number};}
  STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};std::wstring command=L"\""+std::wstring(argv[1])+L"\"";
  require(CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"start debuggee");
  SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_NO_PROMPTS|SYMOPT_FAIL_CRITICAL_ERRORS);require(SymInitializeW(process.hProcess,L"",FALSE),"initialize symbols");
  DWORD64 module=0;unsigned hits=0;DWORD exit=1;
  for(;;){DEBUG_EVENT e{};require(WaitForDebugEvent(&e,30000),"wait debug event");DWORD status=DBG_CONTINUE;
    if(e.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT){module=(DWORD64)e.u.CreateProcessInfo.lpBaseOfImage;require(SymLoadModuleExW(process.hProcess,e.u.CreateProcessInfo.hFile,argv[1],nullptr,module,0,nullptr,0)!=0,"load PDB");CloseHandle(e.u.CreateProcessInfo.hFile);
      require(SymEnumLinesW(process.hProcess,module,nullptr,nullptr,line,&points),"enumerate lines");for(auto& [name,b]:points.points){require(b.address!=0,"find marker");SIZE_T n=0;require(ReadProcessMemory(process.hProcess,(void*)b.address,&b.original,1,&n)&&n==1,"read breakpoint");BYTE trap=0xcc;require(WriteProcessMemory(process.hProcess,(void*)b.address,&trap,1,&n)&&n==1,"set breakpoint");b.active=true;}FlushInstructionCache(process.hProcess,nullptr,0);
    }else if(e.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT&&e.u.LoadDll.hFile)CloseHandle(e.u.LoadDll.hFile);
    else if(e.dwDebugEventCode==EXCEPTION_DEBUG_EVENT){auto& ex=e.u.Exception.ExceptionRecord;bool ours=false;
      for(auto& [name,b]:points.points){if(!b.active||ex.ExceptionCode!=EXCEPTION_BREAKPOINT||(DWORD64)ex.ExceptionAddress!=b.address)continue;ours=true;SIZE_T n=0;require(WriteProcessMemory(process.hProcess,(void*)b.address,&b.original,1,&n)&&n==1,"clear breakpoint");b.active=false;
        HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,e.dwThreadId);require(thread!=nullptr,"open stopped thread");CONTEXT c{};c.ContextFlags=CONTEXT_ALL;require(GetThreadContext(thread,&c),"read thread");c.Rip=b.address;auto vars=locals(process.hProcess,c);
        for(auto& [key,v]:vars)std::printf("%s %s type=%lx flags=%lx reg=%lu addr=%llx size=%lu\n",name.c_str(),key.c_str(),v.type,v.flags,v.reg,v.address,v.size);
        auto get=[&](const char* key)->Var {require(vars.count(key)!=0,key);return vars.at(key);};
        if(name=="VALUES"){
          for(auto [key,value]:std::vector<std::pair<const char*,DWORD64>>{{"value",8},{"parameter",6},{"total",3}})require(scalar(process.hProcess,get(key),c)==value,key);
          require(!vars.count("original")||!available(vars.at("original")),"moved value unavailable");
          auto point=get("point");require(scalar(process.hProcess,member(process.hProcess,module,point,c,L"label"),c)==7,"byte field");require(scalar(process.hProcess,member(process.hProcess,module,point,c,L"x"),c)==42,"aligned int field");require(scalar(process.hProcess,member(process.hProcess,module,point,c,L"y"),c)==0x4004000000000000ull,"double field");
          require(scalar(process.hProcess,member(process.hProcess,module,get("pair"),c,L"second"),c)==13,"generic field");
          auto choice=get("choice");require(scalar(process.hProcess,member(process.hProcess,module,choice,c,L"tag"),c)==1,"enum tag");auto payload=member(process.hProcess,module,choice,c,L"payload");auto pair=member(process.hProcess,module,payload,c,L"Pair");require(scalar(process.hProcess,member(process.hProcess,module,pair,c,L"_1"),c)==19,"enum payload field");
          require(memory(process.hProcess,address(get("numbers"),c)+4,4)==29,"array element");
          auto list=get("list");auto item=member(process.hProcess,module,member(process.hProcess,module,list,c,L"payload"),c,L"Next");auto box=member(process.hProcess,module,item,c,L"_1");auto ptr=member(process.hProcess,module,box,c,L"data");require(memory(process.hProcess,scalar(process.hProcess,ptr,c),8)==0,"recursive pointer target");
        } else if(name=="SHADOW")require(scalar(process.hProcess,get("value"),c)==91,"inner shadow");
        else if(name=="OUTER"||name=="RESULT")require(scalar(process.hProcess,get("value"),c)==8,"outer value");
        else if(name=="INNER"){
          CONTEXT walk=c;STACKFRAME64 frame{};frame.AddrPC={c.Rip,0,AddrModeFlat};frame.AddrFrame={c.Rbp,0,AddrModeFlat};frame.AddrStack={c.Rsp,0,AddrModeFlat};
          for(int depth=0;depth<2;++depth)require(StackWalk64(IMAGE_FILE_MACHINE_AMD64,process.hProcess,thread,&frame,&walk,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)&&frame.AddrPC.Offset,"unwind caller");
          walk.Rip=frame.AddrPC.Offset;walk.Rbp=frame.AddrFrame.Offset;walk.Rsp=frame.AddrStack.Offset;auto caller=locals(process.hProcess,walk);require(caller.count("value")&&scalar(process.hProcess,caller.at("value"),walk)==8,"saved-register variable in caller");
        }
        require(SetThreadContext(thread,&c),"restore thread");CloseHandle(thread);++hits;
      } if(!ours&&ex.ExceptionCode!=EXCEPTION_BREAKPOINT)status=DBG_EXCEPTION_NOT_HANDLED;
    }else if(e.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)exit=e.u.ExitProcess.dwExitCode;
    require(ContinueDebugEvent(e.dwProcessId,e.dwThreadId,status),"continue debuggee");if(e.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)break;
  }
  SymCleanup(process.hProcess);CloseHandle(process.hThread);CloseHandle(process.hProcess);require(exit==0&&hits==5,"all variable breakpoints");std::puts("SYNX_WINDOWS_VARIABLES_OK");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
