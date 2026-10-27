/* Private compiler-cache directories and cross-process entry locks. Policy,
 * fingerprints and manifests live in Synx. Locks release on process exit. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#include <aclapi.h>
#include <sddl.h>
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#endif
/* New directories have a protected ACL. Existing cache directories must have
 * an owner and allow-list limited to this user, SYSTEM, and administrators. */
static int private_directory(wchar_t* path, PSID user) {
  PSID owner=NULL; PACL acl=NULL; PSECURITY_DESCRIPTOR sd=NULL;
  DWORD error=GetNamedSecurityInfoW(path,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&sd);
  if(error || !owner || !EqualSid(owner,user) || !acl){if(sd)LocalFree(sd);return 0;}
  BYTE system[SECURITY_MAX_SID_SIZE],admins[SECURITY_MAX_SID_SIZE];DWORD n=sizeof system,m=sizeof admins;
  int ok=CreateWellKnownSid(WinLocalSystemSid,NULL,system,&n) && CreateWellKnownSid(WinBuiltinAdministratorsSid,NULL,admins,&m);
  for(DWORD i=0;ok && i<acl->AceCount;++i){
    void* item=NULL;if(!GetAce(acl,i,&item)){ok=0;break;}
    ACE_HEADER* header=(ACE_HEADER*)item;
    if(header->AceFlags&INHERIT_ONLY_ACE)continue;
    if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
    if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){ok=0;break;}
    ACCESS_ALLOWED_ACE* ace=(ACCESS_ALLOWED_ACE*)item;PSID sid=&ace->SidStart;
    if(ace->Mask && !EqualSid(sid,user) && !EqualSid(sid,system) && !EqualSid(sid,admins))ok=0;
  }
  LocalFree(sd);return ok;
}
static wchar_t* cache_path(const uint8_t* p, uint64_t n) {
  if (!n || n > INT_MAX || memchr(p, 0, (size_t)n)) return NULL;
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, NULL, 0);
  if (!count) return NULL;
  wchar_t* s = (wchar_t*)malloc(((size_t)count + 1) * sizeof(wchar_t));
  if (!s) return NULL;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, s, count); s[count] = 0;
  for (int i=0;i<count;++i) if (s[i]==L'/') s[i]=L'\\';
  return s;
}
int synx_rt_cache_prepare(const uint8_t* p, uint64_t n) {
  wchar_t* s = cache_path(p,n); if (!s) return EINVAL;
  HANDLE token=NULL;DWORD count=0;TOKEN_USER* info=NULL;LPWSTR sid=NULL;PSECURITY_DESCRIPTOR sd=NULL;int error=EACCES;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))goto done;
  GetTokenInformation(token,TokenUser,NULL,0,&count);
  info=(TOKEN_USER*)malloc(count);if(!info || !GetTokenInformation(token,TokenUser,info,count,&count) || !ConvertSidToStringSidW(info->User.Sid,&sid))goto done;
  size_t length=wcslen(sid);wchar_t* text=(wchar_t*)malloc((length*2+96)*sizeof(wchar_t));if(!text)goto done;
  wcscpy(text,L"O:");wcscat(text,sid);wcscat(text,L"D:P(A;OICI;FA;;;");wcscat(text,sid);wcscat(text,L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
  int valid=ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&sd,NULL);free(text);if(!valid)goto done;
  SECURITY_ATTRIBUTES sa={sizeof sa,sd,FALSE};
  length=wcslen(s);
  for (size_t i=1;i<=length;++i) {
    if (i!=length && s[i]!=L'\\') continue;
    wchar_t saved=s[i];s[i]=0;
    if (i>2 && !CreateDirectoryW(s,&sa) && GetLastError()!=ERROR_ALREADY_EXISTS)goto done;
    s[i]=saved;
  }
  DWORD attr=GetFileAttributesW(s);
  if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT) && private_directory(s,info->User.Sid))error=0;
done:
  if(sd)LocalFree(sd);if(sid)LocalFree(sid);free(info);if(token)CloseHandle(token);free(s);return error;
}
int synx_rt_cache_lock(const uint8_t* p, uint64_t n, void** out) {
  *out=NULL; wchar_t* s=cache_path(p,n); if (!s) return EINVAL;
  HANDLE h=CreateFileW(s,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); free(s);
  if (h==INVALID_HANDLE_VALUE) return EACCES;
  OVERLAPPED ov; memset(&ov,0,sizeof ov);
  if (!LockFileEx(h,LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&ov)) { CloseHandle(h); return EACCES; }
  *out=h; return 0;
}
void synx_rt_cache_unlock(void* p) { if (p) { OVERLAPPED ov; memset(&ov,0,sizeof ov); UnlockFileEx((HANDLE)p,0,1,0,&ov); CloseHandle((HANDLE)p); } }
uint64_t synx_rt_cache_nonce(void) { static volatile LONG counter; return ((uint64_t)GetCurrentProcessId()<<32) ^ GetTickCount64() ^ (uint32_t)InterlockedIncrement(&counter); }
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
static char* cache_path(const uint8_t* p,uint64_t n) {
  if (!n || n>SIZE_MAX-1 || memchr(p,0,(size_t)n)) return NULL;
  char* s=(char*)malloc((size_t)n+1); if(s){ memcpy(s,p,(size_t)n);s[n]=0; } return s;
}
int synx_rt_cache_prepare(const uint8_t* p,uint64_t n) {
  char* s=cache_path(p,n); if(!s)return EINVAL;
  for(size_t i=1;i<=n;++i) {
    if(i!=n && s[i]!='/')continue;
    char saved=s[i];s[i]=0;
    if(mkdir(s,0700) && errno!=EEXIST){int e=errno;free(s);return e;} s[i]=saved;
  }
  struct stat st; int e=0;
  if(lstat(s,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid())e=EACCES;
  else if(st.st_mode & 0022)e=EACCES;
  free(s);return e;
}
int synx_rt_cache_lock(const uint8_t* p,uint64_t n,void** out) {
  *out=NULL;char* s=cache_path(p,n);if(!s)return EINVAL;
  int fd=open(s,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);free(s);if(fd<0)return errno;
  struct stat st;
  if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || st.st_nlink!=1){close(fd);return EACCES;}
  int rc;do {rc=flock(fd,LOCK_EX);}while(rc && errno==EINTR);
  if(rc){int e=errno;close(fd);return e;} *out=(void*)(intptr_t)(fd+1);return 0;
}
void synx_rt_cache_unlock(void* p){if(p){int fd=(int)(intptr_t)p-1;flock(fd,LOCK_UN);close(fd);}}
uint64_t synx_rt_cache_nonce(void){static uint32_t counter;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ((uint64_t)getpid()<<32)^(uint64_t)ts.tv_nsec^(uint64_t)ts.tv_sec^(++counter);}
#endif

/* SHA-256 for cache content identities. Unsigned arithmetic is modulo 2^32.
 * The platform-neutral implementation is checked against Python hashlib and
 * the Synx Mach-O signer in the workflow tests. */
static uint32_t rotr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
void synx_rt_cache_hash(const uint8_t* input,uint64_t size,uint8_t* output){
  static const uint32_t k[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  uint64_t blocks=size/64+1+(size%64>=56); uint64_t bits=size*8;
  for(uint64_t block=0;block<blocks;++block){
    uint32_t w[64];for(unsigned j=0;j<16;++j){uint32_t value=0;for(unsigned b=0;b<4;++b){uint64_t at=block*64+j*4+b;uint8_t c=at<size?input[at]:at==size?128:at>=blocks*64-8?(uint8_t)(bits>>((blocks*64-at-1)*8)):0;value=(value<<8)|c;}w[j]=value;}
    for(unsigned j=16;j<64;++j){uint32_t a=w[j-15],b=w[j-2];w[j]=w[j-16]+(rotr(a,7)^rotr(a,18)^(a>>3))+w[j-7]+(rotr(b,17)^rotr(b,19)^(b>>10));}
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
    for(unsigned j=0;j<64;++j){uint32_t t=z+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[j]+w[j],u=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));z=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
  }
  for(unsigned j=0;j<32;++j)output[j]=(uint8_t)(h[j/4]>>(24-(j%4)*8));
}

#if defined(SYNX_STANDALONE_RUNTIME)
#include "exports_cache.inc"
#endif
