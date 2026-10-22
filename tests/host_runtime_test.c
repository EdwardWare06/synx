/* Check native descriptor flags and actual inheritance through the adapter. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif
extern uint64_t synx_rt_arg_count(void);
extern const uint8_t* synx_rt_arg_data(uint64_t);
extern int synx_rt_file_open(const uint8_t*, uint64_t, int, FILE**);
extern int synx_rt_command(const uint8_t*, uint64_t, uint64_t, int64_t*);
extern int synx_rt_absolute(const uint8_t*, uint64_t, uint8_t*, uint64_t*);
static int absolute_checks(void) {
  uint64_t count = 0;
  uint8_t guard = 91;
  if (synx_rt_absolute((const uint8_t*)"missing-target", 14, &guard, &count) != -1 || !count || guard != 91) return 1;
  uint8_t* path = (uint8_t*)malloc((size_t)count);
  if (!path) return 1;
  uint64_t cap = count;
  int failed = synx_rt_absolute((const uint8_t*)"missing-target", 14, path, &count) != 0 || count != cap;
  free(path);
  count = 1;
  if (synx_rt_absolute((const uint8_t*)"", 0, &guard, &count) != EINVAL || count || guard != 91) failed = 1;
  count = 1;
  if (synx_rt_absolute((const uint8_t*)"a\0b", 3, &guard, &count) != EINVAL || count || guard != 91) failed = 1;
#if !defined(_WIN32)
  char* cwd = getcwd(NULL, 0);
  if (!cwd) return 1;
  if (chdir("/")) { free(cwd); return 1; }
  uint8_t root_path[32]; count = sizeof root_path;
  int rc = synx_rt_absolute((const uint8_t*)"missing-target", 14, root_path, &count);
  if (chdir(cwd)) { free(cwd); return 1; }
  free(cwd);
  if (rc || count != 15 || memcmp(root_path, "/missing-target", 15)) failed = 1;
#endif
  return failed;
}
void synx_thread_entry(void* unused) { (void)unused; abort(); }
int synx_entry(void) {
  if (synx_rt_arg_count() == 4) {
    uint64_t number = strtoull((const char*)synx_rt_arg_data(2), NULL, 10);
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION actual, expected;
    if (!GetFileInformationByHandle((HANDLE)(uintptr_t)number, &actual)) return 0;
    HANDLE file = CreateFileA((const char*)synx_rt_arg_data(3), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 1;
    BOOL ok = GetFileInformationByHandle(file, &expected); CloseHandle(file);
    if (!ok) return 1;
    return actual.dwVolumeSerialNumber == expected.dwVolumeSerialNumber && actual.nFileIndexHigh == expected.nFileIndexHigh && actual.nFileIndexLow == expected.nFileIndexLow;
#else
    struct stat actual, expected;
    if (fstat((int)number, &actual)) return errno == EBADF ? 0 : 1;
    if (stat((const char*)synx_rt_arg_data(3), &expected)) return 1;
    return actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
#endif
  }
  if (absolute_checks()) return 1;
  const uint8_t* program = synx_rt_arg_data(0);
  char name[96];
#if defined(_WIN32)
  snprintf(name, sizeof name, "synx-host-inherit-%lu.tmp", (unsigned long)GetCurrentProcessId());
#else
  snprintf(name, sizeof name, "synx-host-inherit-%ld.tmp", (long)getpid());
#endif
  FILE* file = NULL;
  if (synx_rt_file_open((const uint8_t*)name, strlen(name), 1, &file)) return 1;
  int failed = 0;
  uint64_t number;
#if defined(_WIN32)
  HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
  DWORD flags = 0;
  if (!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT)) failed = 1;
  /* Even an explicitly inheritable unrelated handle must be excluded by
   * the process adapter's standard-stream handle list. */
  if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) failed = 1;
  number = (uint64_t)(uintptr_t)handle;
#else
  int descriptor = fileno(file);
  int flags = fcntl(descriptor, F_GETFD);
  if (flags < 0 || !(flags & FD_CLOEXEC)) failed = 1;
  number = (uint64_t)descriptor;
#endif
  char argument[32]; snprintf(argument, sizeof argument, "%llu", (unsigned long long)number);
  size_t pn = strlen((const char*)program) + 1, an = strlen(argument) + 1;
  size_t nn = strlen(name) + 1;
  size_t size = pn + 8 + an + nn;
  uint8_t* blob = (uint8_t*)malloc(size);
  if (!blob) { fclose(file); remove(name); return 1; }
  memcpy(blob, program, pn); memcpy(blob + pn, "--probe", 8); memcpy(blob + pn + 8, argument, an); memcpy(blob + pn + 8 + an, name, nn);
  int64_t status = -1;
  if (synx_rt_command(blob, size, 4, &status) || status) failed = 1;
  free(blob); fclose(file); remove(name);
  if (failed) fputs("host descriptor inheritance failed\n", stderr);
  return failed;
}
