/* A panic terminates without invoking C exit hooks. The hook makes a normal
 * CRT exit observable, including when a worker requests process termination. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

void (*synx_link_thread_entry)(void*) = NULL;
extern void synx_rt_panic(const uint8_t*, uint64_t);
static void on_exit_hook(void) { _Exit(87); }
static void panic_now(void) { synx_rt_panic((const uint8_t*)"termination probe", 17); }
#if defined(_WIN32)
static DWORD WINAPI worker(void* unused) { (void)unused; panic_now(); return 0; }
#else
static void* worker(void* unused) { (void)unused; panic_now(); return NULL; }
#endif
int main(int argc, char** argv) {
  if(argc != 2 || atexit(on_exit_hook)) return 2;
  if(!strcmp(argv[1], "normal")) return 0;
  if(!strcmp(argv[1], "main")) panic_now();
  if(strcmp(argv[1], "worker")) return 2;
#if defined(_WIN32)
  HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  if(!thread) return 3;
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
#else
  pthread_t thread;
  if(pthread_create(&thread, NULL, worker, NULL)) return 3;
  pthread_join(thread, NULL);
#endif
  return 4;
}
