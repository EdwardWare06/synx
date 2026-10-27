/* Single-file native startup. The release toolchain links this host to the
 * platform CRT once. Synx resolves the appended native image and writes only
 * relative addresses. No library is extracted and no compiler is used here.
 * All input ranges and protection boundaries are checked before execution. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "exports_catalogue.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#endif
#define IMAGE_LIMIT UINT64_C(268435456)
#define IMAGE_ALIGN UINT64_C(65536)
typedef struct {
  uint8_t magic[32]; uint32_t version, os;
  uint64_t offset, size; uint8_t abi[32]; uint64_t names_size;
  char names[SYNX_EXPORT_NAMES_SIZE];
} Locator;
/* volatile prevents constant folding of fields patched by the Synx linker. */
static const volatile Locator locator = {
 {83,89,78,88,45,78,65,84,73,86,69,45,84,69,77,80,76,65,84,69,45,86,49,45,88,54,52,45,76,79,67,0}, 1,
#if defined(_WIN32)
 1,
#elif defined(__APPLE__)
 2,
#else
 0,
#endif
 0,0,SYNX_EXPORT_ABI,SYNX_EXPORT_NAMES_SIZE,SYNX_EXPORT_NAMES
};
extern int synx_rt_exe_path(uint8_t*,uint64_t,uint64_t*);
extern int synx_rt_file_open(const uint8_t*,uint64_t,int,void**);
extern int synx_rt_file_close(void*);
extern int synx_rt_file_seek(void*,int64_t,int);
extern int synx_rt_file_read(void*,uint8_t*,uint64_t,uint64_t*);
extern void* synx_export_rt(const char*);
extern void* synx_export_threads(const char*);
extern void* synx_export_host(const char*);
extern void* synx_export_driver(const char*);
extern void* synx_export_link(const char*);
extern void* synx_export_cache(const char*);
static void* resolve_export(const char* name) {
  void* p;
  if((p=synx_export_rt(name)))return p;
  if((p=synx_export_threads(name)))return p;
  if((p=synx_export_host(name)))return p;
  if((p=synx_export_driver(name)))return p;
  if((p=synx_export_link(name)))return p;
  return synx_export_cache(name);
}
static uint64_t word(const uint8_t* p) { uint64_t v=0; for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(i*8); return v; }
static int range(uint64_t at,uint64_t n,uint64_t limit){return at<=limit && n<=limit-at;}
typedef struct { uint64_t rva,size,count,offset,protection; } Segment;
static int in_segment(const Segment* s,unsigned n,uint64_t at,uint64_t count,unsigned protection) {
  for(unsigned i=0;i<n;++i)if((!protection || s[i].protection==protection) && at>=s[i].rva && range(at-s[i].rva,count,s[i].size))return 1;
  return 0;
}
static uint8_t* read_payload(uint64_t offset,uint64_t size) {
  if(!offset || !size || size>IMAGE_LIMIT || offset>INT64_MAX)return NULL;
  void* file=NULL;int e;
#if defined(__linux__)
  /* This opens the running inode even after its directory entry is replaced. */
  e=synx_rt_file_open((const uint8_t*)"/proc/self/exe",14,0,&file);if(e)return NULL;
#else
  uint64_t capacity=512,count=0; uint8_t* path=NULL;
  for(;;) {
    uint8_t* next=(uint8_t*)realloc(path,(size_t)capacity); if(!next){free(path);return NULL;}path=next;
    e=synx_rt_exe_path(path,capacity,&count);
    if(e==-1 && count>capacity && count<=1048576){capacity=count;continue;}
    if(e){free(path);return NULL;}break;
  }
  e=synx_rt_file_open(path,count,0,&file);free(path);if(e)return NULL;
#endif
  uint8_t* data=(uint8_t*)malloc((size_t)size); if(!data){synx_rt_file_close(file);return NULL;}
  e=synx_rt_file_seek(file,(int64_t)offset,0);uint64_t at=0;
  while(!e && at<size){uint64_t got=0;e=synx_rt_file_read(file,data+at,size-at,&got);if(!got)e=1;at+=got;}
  synx_rt_file_close(file);if(e){free(data);return NULL;}return data;
}
static int load_program(int (**entry)(void),void (**thread)(void*)) {
  uint64_t size=locator.size;uint8_t* data=read_payload(locator.offset,size);if(!data)return 0;
  int ok=0;uint8_t* image=NULL;uint64_t image_size=0;
  if(size<88 || memcmp(data,"SNXPAY01",8))goto done;
  for(unsigned i=0;i<32;++i)if(data[8+i]!=locator.abi[i])goto done;
  image_size=word(data+40);uint64_t entry_rva=word(data+48),thread_rva=word(data+56);
  uint64_t nseg=word(data+64),nrel=word(data+72),nimp=word(data+80);
  if(!image_size || image_size>IMAGE_LIMIT || image_size%IMAGE_ALIGN || nseg!=4 || nrel>size/8 || nimp>65536)goto done;
  Segment segments[4];uint64_t at=88,previous=0;
  for(unsigned i=0;i<4;++i) {
    if(!range(at,40,size))goto done;
    Segment* s=&segments[i];
    s->rva=word(data+at);s->size=word(data+at+8);s->count=word(data+at+16);s->offset=word(data+at+24);s->protection=word(data+at+32);at+=40;
    if(s->rva%IMAGE_ALIGN || s->rva<IMAGE_ALIGN || s->rva<previous || !range(s->rva,s->size,image_size) || s->count>s->size || !range(s->offset,s->count,size))goto done;
    if(s->protection!=(i==0?5:i==1?1:3) || (i==3 && s->count))goto done;
    uint64_t rounded=(s->size+IMAGE_ALIGN-1)&~(IMAGE_ALIGN-1);if(!rounded)rounded=IMAGE_ALIGN;
    if(!range(s->rva,rounded,image_size))goto done;
    previous=s->rva+rounded;
  }
  if(!in_segment(segments,4,entry_rva,1,5) || !in_segment(segments,4,thread_rva,1,5) || !range(at,nrel*8,size))goto done;
  uint64_t reloc_at=at;at+=nrel*8;uint64_t import_at=at;
  /* Validate all names/slots and relocation sites before allocating executable memory. */
  previous=0;
  for(uint64_t i=0;i<nrel;++i){uint64_t site=word(data+reloc_at+i*8);if((i && site<previous) || !in_segment(segments,4,site,8,0))goto done;previous=site+8;}
  for(uint64_t i=0;i<nimp;++i){
    if(!range(at,16,size))goto done;
    uint64_t slot=word(data+at),n=word(data+at+8);at+=16;
    if(!n || n>255 || slot%8 || !in_segment(segments,4,slot,8,3) || !range(at,n,size) || memchr(data+at,0,(size_t)n))goto done;
    char name[256];memcpy(name,data+at,(size_t)n);name[n]=0;if(!resolve_export(name))goto done;at+=n;
  }
  for(unsigned i=0;i<4;++i)if(segments[i].count && segments[i].offset<at)goto done;
#if defined(_WIN32)
  image=(uint8_t*)VirtualAlloc(NULL,(SIZE_T)image_size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
  {long page=sysconf(_SC_PAGESIZE);if(page<=0 || IMAGE_ALIGN%(uint64_t)page)goto done;}
  image=(uint8_t*)mmap(NULL,(size_t)image_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(image==MAP_FAILED)image=NULL;
#endif
  if(!image)goto done;
  for(unsigned i=0;i<4;++i)if(segments[i].count)memcpy(image+segments[i].rva,data+segments[i].offset,(size_t)segments[i].count);
  for(uint64_t i=0;i<nrel;++i){uint64_t site=word(data+reloc_at+i*8),value=word(image+site);if(value>image_size)goto done;value+=(uint64_t)(uintptr_t)image;memcpy(image+site,&value,8);}
  at=import_at;
  for(uint64_t i=0;i<nimp;++i){uint64_t slot=word(data+at),n=word(data+at+8);at+=16;char name[256];memcpy(name,data+at,(size_t)n);name[n]=0;at+=n;void* address=resolve_export(name);memcpy(image+slot,&address,8);}
  /* No page is writable and executable at the same time. */
  for(unsigned i=0;i<2;++i){Segment* s=&segments[i];if(!s->size)continue;
#if defined(_WIN32)
    DWORD old;if(!VirtualProtect(image+s->rva,(SIZE_T)s->size,i==0?PAGE_EXECUTE_READ:PAGE_READONLY,&old))goto done;
#else
    if(mprotect(image+s->rva,(size_t)s->size,i==0?PROT_READ|PROT_EXEC:PROT_READ))goto done;
#endif
  }
#if defined(_WIN32)
  if(!FlushInstructionCache(GetCurrentProcess(),image,(SIZE_T)image_size))goto done;
#else
  __builtin___clear_cache((char*)image,(char*)image+image_size);
#endif
  { void* code=image+entry_rva;memcpy(entry,&code,sizeof code);code=image+thread_rva;memcpy(thread,&code,sizeof code); }
  ok=1;
done:
  free(data);
  if(!ok && image){
#if defined(_WIN32)
    VirtualFree(image,0,MEM_RELEASE);
#else
    munmap(image,(size_t)image_size);
#endif
  }
  /* The process owns a successful mapping through all worker callbacks. */
  return ok;
}
#if defined(_WIN32)
extern int synx_rt_link_start_v1(int(*)(void),void(*)(void*));
int wmain(int argc,wchar_t** argv){(void)argc;(void)argv;int(*entry)(void);void(*thread)(void*);if(!load_program(&entry,&thread)){fputs("Synx startup: invalid standalone image\n",stderr);return 126;}return synx_rt_link_start_v1(entry,thread);}
#else
extern int synx_rt_link_start_v1(int,char**,int(*)(void),void(*)(void*));
int main(int argc,char** argv){int(*entry)(void);void(*thread)(void*);if(!load_program(&entry,&thread)){fputs("Synx startup: invalid standalone image\n",stderr);return 126;}return synx_rt_link_start_v1(argc,argv,entry,thread);}
#endif
