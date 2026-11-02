// Emulate actual Axon prologue instructions and ask Windows to unwind at each
// boundary. The independent instruction decoder checks the emitted unwind data.
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
static DWORD64 reg(const CONTEXT& c,unsigned i) { DWORD64 v;std::memcpy(&v,reinterpret_cast<const BYTE*>(&c)+offsetof(CONTEXT,Rax)+i*8,8);return v; }
static void reg(CONTEXT& c,unsigned i,DWORD64 v) { std::memcpy(reinterpret_cast<BYTE*>(&c)+offsetof(CONTEXT,Rax)+i*8,&v,8); }
static void require_unwind(bool ok) { if(!ok)throw std::runtime_error("Windows virtual unwind mismatch"); }
static DWORD64 preferred_base=0;
static unsigned virtual_unwind(const wchar_t* file) {
  HANDLE raw=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(raw==INVALID_HANDLE_VALUE)fail("read PE preferred base");
  BYTE header[4096]{};DWORD length=0;if(!ReadFile(raw,header,sizeof(header),&length,nullptr))fail("read PE header");CloseHandle(raw);
  auto raw_dos=reinterpret_cast<IMAGE_DOS_HEADER*>(header);require_unwind(length>=64&&raw_dos->e_lfanew>0&&DWORD(raw_dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)<=length);
  preferred_base=reinterpret_cast<IMAGE_NT_HEADERS64*>(header+raw_dos->e_lfanew)->OptionalHeader.ImageBase;
  auto module=LoadLibraryExW(file,nullptr,DONT_RESOLVE_DLL_REFERENCES);if(!module)fail("map unwind fixture");
  auto base=reinterpret_cast<BYTE*>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
  auto sections=IMAGE_FIRST_SECTION(nt);DWORD start=0,end=0;
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(!std::memcmp(sections[i].Name,".snxtext",8)){start=sections[i].VirtualAddress;end=start+sections[i].Misc.VirtualSize;}
  if(!start){FreeLibrary(module);return 0;}
  auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];auto table=reinterpret_cast<RUNTIME_FUNCTION*>(base+directory.VirtualAddress);unsigned checks=0;
  for(unsigned f=0;f<directory.Size/sizeof(RUNTIME_FUNCTION);++f) {
    auto& function=table[f];if(function.BeginAddress<start||function.BeginAddress>=end)continue;
    BYTE* code=base+function.BeginAddress;BYTE* info=base+function.UnwindData;require_unwind(info[0]==1&&info[3]==0);
    DWORD64 allocation=0;std::vector<unsigned> saved;
    for(unsigned i=0;i<info[2];++i){auto at=info+4+i*2;unsigned op=at[1]&15,high=at[1]>>4;
      if(op==0)saved.push_back(high);
      else if(op==1){if(high==0){WORD n;std::memcpy(&n,at+2,2);allocation=n*8;++i;}else{DWORD n;std::memcpy(&n,at+2,4);allocation=n;i+=2;}}
      else if(op==2)allocation=high*8+8;
      else if(op==4){saved.push_back(high);++i;}
      else if(op==5){saved.push_back(high);i+=2;}
      else require_unwind(false);
    }
    require_unwind(allocation<16*1024*1024);
    std::vector<BYTE> storage(static_cast<size_t>(allocation)+8192);
    auto entry=((reinterpret_cast<DWORD64>(storage.data()+storage.size()-256))&~DWORD64(15))+8;
    const DWORD64 return_address=0x12345678;std::memcpy(reinterpret_cast<void*>(entry),&return_address,8);
    CONTEXT context{};context.ContextFlags=CONTEXT_FULL;for(unsigned i=0;i<16;++i)reg(context,i,0x12340000+i*0x100);context.Rsp=entry;
    CONTEXT initial=context;
    auto check_context=[&](const CONTEXT& actual,DWORD offset){CONTEXT unwound=actual;unwound.Rip=reinterpret_cast<DWORD64>(code+offset);PVOID handler=nullptr;DWORD64 establisher=0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER,reinterpret_cast<DWORD64>(base),unwound.Rip,&function,&unwound,&handler,&establisher,nullptr);
      require_unwind(unwound.Rip==return_address&&unwound.Rsp==entry+8);
      for(unsigned i:{3u,5u,6u,7u,12u,13u,14u,15u})require_unwind(reg(unwound,i)==reg(initial,i));++checks;
    };
    DWORD pc=0;unsigned iterations=0;bool above=false;
    while(pc<info[1]) {
      check_context(context,pc);require_unwind(++iterations<100000);
      if(code[pc]==0x55){context.Rsp-=8;std::memcpy(reinterpret_cast<void*>(context.Rsp),&context.Rbp,8);++pc;continue;}
      if(code[pc]==0x0f&&code[pc+1]==0x83){int displacement;std::memcpy(&displacement,code+pc+2,4);pc=above?DWORD(pc+6+displacement):pc+6;continue;}
      BYTE rex=code[pc],op=code[pc+1],mr=code[pc+2];require_unwind((rex&0xf8)==0x48);
      unsigned src=((mr>>3)&7)+((rex&4)?8:0),dst=(mr&7)+((rex&1)?8:0);
      if(op==0x89&&mr>=0xc0){reg(context,dst,reg(context,src));pc+=3;}
      else if(op==0x89&&(mr&0xc7)==0x85){int displacement;std::memcpy(&displacement,code+pc+3,4);auto value=reg(context,src);std::memcpy(reinterpret_cast<void*>(context.Rbp+displacement),&value,8);pc+=7;}
      else if((op==0x81||op==0x83)&&mr>=0xc0){int value=0;if(op==0x81)std::memcpy(&value,code+pc+3,4);else value=static_cast<signed char>(code[pc+3]);require_unwind(src==5);reg(context,dst,reg(context,dst)-value);pc+=op==0x81?7:4;}
      else if(op==0x8d&&(mr&0xc0)==0x80){int displacement;std::memcpy(&displacement,code+pc+3,4);reg(context,src,reg(context,dst)+displacement);pc+=7;}
      else if(op==0x8b&&(mr&0xc0)==0x80){int displacement;std::memcpy(&displacement,code+pc+3,4);DWORD64 value;std::memcpy(&value,reinterpret_cast<void*>(reg(context,dst)+displacement),8);reg(context,src,value);pc+=7;}
      else if(op==0x39&&mr>=0xc0){above=reg(context,dst)>=reg(context,src);pc+=3;}
      else require_unwind(false);
    }
    check_context(context,pc);
    CONTEXT body=context;for(auto i:saved)reg(body,i,0xabcdef00+i);check_context(body,pc);
    DWORD tail=function.EndAddress-function.BeginAddress;
    require_unwind(tail>=6&&code[tail-1]==0xc3&&code[tail-2]==0x5d);
    DWORD epilogue=tail-6;if(code[epilogue]!=0x48||code[epilogue+1]!=0x83||code[epilogue+2]!=0xc4)epilogue=tail-9;
    require_unwind(code[epilogue]==0x48&&(code[epilogue+1]==0x83||code[epilogue+1]==0x81)&&code[epilogue+2]==0xc4);
    check_context(context,epilogue);context.Rsp+=allocation;check_context(context,tail-2);std::memcpy(&context.Rbp,reinterpret_cast<void*>(context.Rsp),8);context.Rsp+=8;check_context(context,tail-1);
  }
  FreeLibrary(module);require_unwind(checks>0);return checks;
}
