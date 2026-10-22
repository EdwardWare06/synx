/* Host operations for a Synx compiler: argv execution, environment reads,
 * executable location, and filesystem identity. Policy and ownership live
 * in Synx. Return errno, -1 for a short buffer, or -2 for a missing variable. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#else
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

extern void synx_rt_blocking_check(void* pool);

static int copy_bytes(const char* text, uint8_t* out, uint64_t cap, uint64_t* count) {
  *count = (uint64_t)strlen(text);
  if (cap < *count) return -1;
  if (*count) memcpy(out, text, (size_t)*count);
  return 0;
}

#if defined(_WIN32)
static int windows_error(DWORD code) {
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_DRIVE) return ENOENT;
  if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) return EACCES;
  if (code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY) return ENOMEM;
  if (code == ERROR_BAD_EXE_FORMAT) return ENOEXEC;
  if (code == ERROR_FILENAME_EXCED_RANGE) return ENAMETOOLONG;
  if (code == ERROR_INVALID_PARAMETER || code == ERROR_INVALID_NAME) return EINVAL;
  return EIO;
}
static wchar_t* wide_text(const uint8_t* text, uint64_t length) {
  if (length > INT_MAX) { errno = E2BIG; return NULL; }
  int n = length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)text, (int)length, NULL, 0) : 0;
  if (length && !n) { errno = EINVAL; return NULL; }
  wchar_t* result = (wchar_t*)malloc(((size_t)n + 1) * sizeof(wchar_t));
  if (!result) { errno = ENOMEM; return NULL; }
  if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)text, (int)length, result, n);
  result[n] = 0;
  return result;
}
static int copy_wide(const wchar_t* text, uint8_t* out, uint64_t cap, uint64_t* count) {
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
  if (!n) return EILSEQ;
  *count = (uint64_t)n - 1;
  if (cap < *count) return -1;
  char* bytes = (char*)malloc((size_t)n);
  if (!bytes) return ENOMEM;
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, bytes, n, NULL, NULL)) { free(bytes); return EILSEQ; }
  if (*count) memcpy(out, bytes, (size_t)*count);
  free(bytes);
  return 0;
}
#else
static char* native_text(const uint8_t* text, uint64_t length) {
  if (length >= SIZE_MAX) { errno = E2BIG; return NULL; }
  char* result = (char*)malloc((size_t)length + 1);
  if (!result) { errno = ENOMEM; return NULL; }
  if (length) memcpy(result, text, (size_t)length);
  result[length] = 0;
  return result;
}
#endif

/* count is buffer capacity on entry and byte length on return, excluding NUL.
 * -1 requests a larger buffer; -2 means missing; zero includes an empty value.
 * The adapter retains neither the name nor the output buffer. */
int synx_rt_env_read(const uint8_t* name, uint64_t length, uint8_t* out, uint64_t* count) {
  uint64_t cap = *count;
  *count = 0;
  if (!length || length > SIZE_MAX || memchr(name, 0, (size_t)length) || memchr(name, '=', (size_t)length)) return EINVAL;
#if defined(_WIN32)
  wchar_t* key = wide_text(name, length);
  if (!key) return errno;
  DWORD size = 128;
  for (;;) {
    wchar_t* value = (wchar_t*)malloc((size_t)size * sizeof(wchar_t));
    if (!value) { free(key); return ENOMEM; }
    SetLastError(ERROR_SUCCESS);
    DWORD n = GetEnvironmentVariableW(key, value, size);
    DWORD error = GetLastError();
    if (!n && error != ERROR_SUCCESS) {
      free(value); free(key);
      return error == ERROR_ENVVAR_NOT_FOUND ? -2 : windows_error(error);
    }
    if (n >= size) { free(value); size = n; continue; }
    value[n] = 0;
    int rc = copy_wide(value, out, cap, count);
    free(value); free(key); return rc;
  }
#else
  char* key = native_text(name, length);
  if (!key) return errno;
  const char* value = getenv(key);
  int rc = value ? copy_bytes(value, out, cap, count) : -2;
  free(key); return rc;
#endif
}

/* Writes the executable path as UTF-8, without NUL. A short buffer returns
 * -1 and a larger required count. Other failures return errno. */
int synx_rt_exe_path(uint8_t* out, uint64_t cap, uint64_t* count) {
  *count = 0;
#if defined(_WIN32)
  DWORD size = 256;
  for (;;) {
    wchar_t* path = (wchar_t*)malloc((size_t)size * sizeof(wchar_t));
    if (!path) return ENOMEM;
    DWORD n = GetModuleFileNameW(NULL, path, size);
    if (!n) { int error = windows_error(GetLastError()); free(path); return error; }
    if (n == size) { free(path); if (size > MAXDWORD / 2) return ENAMETOOLONG; size *= 2; continue; }
    int rc = copy_wide(path, out, cap, count); free(path); return rc;
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(NULL, &size);
  char* path = (char*)malloc(size);
  if (!path) return ENOMEM;
  if (_NSGetExecutablePath(path, &size)) { free(path); return EIO; }
  char* absolute = realpath(path, NULL); free(path);
  if (!absolute) return errno;
  int rc = copy_bytes(absolute, out, cap, count); free(absolute); return rc;
#else
  if (!cap || cap > SIZE_MAX || cap > INT64_MAX) return EINVAL;
  ssize_t n = readlink("/proc/self/exe", (char*)out, (size_t)cap);
  if (n < 0) return errno;
  if ((uint64_t)n == cap) { if (cap > UINT64_MAX / 2) return ENAMETOOLONG; *count = cap * 2; return -1; }
  *count = (uint64_t)n; return 0;
#endif
}

#if defined(_WIN32)
static HANDLE path_handle(const uint8_t* text, uint64_t length) {
  wchar_t* path = wide_text(text, length);
  if (!path) return INVALID_HANDLE_VALUE;
  HANDLE handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) errno = windows_error(GetLastError());
  free(path); return handle;
}
#endif
/* Makes a path absolute without resolving links or requiring a target.
 * Windows uses per-drive current directories and GetFullPathNameW semantics.
 * POSIX retains dot components. count uses the env_read capacity protocol. */
int synx_rt_absolute(const uint8_t* text, uint64_t length, uint8_t* out, uint64_t* count) {
  uint64_t cap = *count;
  *count = 0;
  if (!length || length > SIZE_MAX || memchr(text, 0, (size_t)length)) return EINVAL;
#if defined(_WIN32)
  wchar_t* input = wide_text(text, length);
  if (!input) return errno;
  DWORD size = 256;
  for (;;) {
    wchar_t* path = (wchar_t*)malloc((size_t)size * sizeof(wchar_t));
    if (!path) { free(input); return ENOMEM; }
    DWORD n = GetFullPathNameW(input, size, path, NULL);
    if (!n) { int error = windows_error(GetLastError()); free(path); free(input); return error; }
    if (n >= size) { free(path); if (n == MAXDWORD) { free(input); return ENAMETOOLONG; } size = n + 1; continue; }
    int rc = copy_wide(path, out, cap, count);
    free(path); free(input); return rc;
  }
#else
  char* input = native_text(text, length);
  if (!input) return errno;
  if (input[0] == '/') { int rc = copy_bytes(input, out, cap, count); free(input); return rc; }
  size_t size = 256;
  char* cwd;
  for (;;) {
    cwd = (char*)malloc(size);
    if (!cwd) { free(input); return ENOMEM; }
    if (getcwd(cwd, size)) break;
    int error = errno; free(cwd);
    if (error != ERANGE || size > SIZE_MAX / 2) { free(input); return error; }
    size *= 2;
  }
  size_t n = strlen(cwd);
  if (n > SIZE_MAX - 2 || length > SIZE_MAX - n - 2) { free(cwd); free(input); return ENAMETOOLONG; }
  size_t separator = n && cwd[n - 1] == '/' ? 0 : 1;
  char* path = (char*)malloc(n + (size_t)length + 2);
  if (!path) { free(cwd); free(input); return ENOMEM; }
  memcpy(path, cwd, n); if (separator) path[n] = '/';
  memcpy(path + n + separator, input, (size_t)length + 1);
  int rc = copy_bytes(path, out, cap, count);
  free(path); free(cwd); free(input); return rc;
#endif
}

/* Resolves an existing target and follows links. count uses the env_read
 * capacity/length protocol; the returned path has no terminating NUL. */
int synx_rt_canonical(const uint8_t* text, uint64_t length, uint8_t* out, uint64_t* count) {
  uint64_t cap = *count;
  *count = 0;
  if (!length) return ENOENT;
  if (length > SIZE_MAX || memchr(text, 0, (size_t)length)) return EINVAL;
#if defined(_WIN32)
  HANDLE handle = path_handle(text, length);
  if (handle == INVALID_HANDLE_VALUE) return errno;
  DWORD size = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
  if (!size || size == MAXDWORD) { int error = windows_error(GetLastError()); CloseHandle(handle); return error; }
  wchar_t* path = (wchar_t*)malloc(((size_t)size + 1) * sizeof(wchar_t));
  if (!path) { CloseHandle(handle); return ENOMEM; }
  DWORD n = GetFinalPathNameByHandleW(handle, path, size + 1, FILE_NAME_NORMALIZED);
  int rc = !n ? windows_error(GetLastError()) : n > size ? EIO : copy_wide(path, out, cap, count);
  free(path); CloseHandle(handle); return rc;
#else
  char* path = native_text(text, length);
  if (!path) return errno;
  char* absolute = realpath(path, NULL);
  int error = errno; free(path);
  if (!absolute) return error;
  int rc = copy_bytes(absolute, out, cap, count); free(absolute); return rc;
#endif
}

static int same_file(const uint8_t* a, uint64_t an, const uint8_t* b, uint64_t bn, int* equal) {
  *equal = 0;
  if (!an || !bn) return ENOENT;
  if (an > SIZE_MAX || bn > SIZE_MAX || memchr(a, 0, (size_t)an) || memchr(b, 0, (size_t)bn)) return EINVAL;
#if defined(_WIN32)
  HANDLE left = path_handle(a, an);
  if (left == INVALID_HANDLE_VALUE) return errno;
  HANDLE right = path_handle(b, bn);
  if (right == INVALID_HANDLE_VALUE) { int error = errno; CloseHandle(left); return error; }
  BY_HANDLE_FILE_INFORMATION x, y;
  int rc = 0;
  if (!GetFileInformationByHandle(left, &x) || !GetFileInformationByHandle(right, &y)) rc = windows_error(GetLastError());
  else *equal = x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
  CloseHandle(left); CloseHandle(right); return rc;
#else
  char* left = native_text(a, an);
  if (!left) return errno;
  char* right = native_text(b, bn);
  if (!right) { int error = errno; free(left); return error; }
  struct stat x, y;
  int rc = stat(left, &x) || stat(right, &y) ? errno : 0;
  if (!rc) *equal = x.st_dev == y.st_dev && x.st_ino == y.st_ino;
  free(left); free(right); return rc;
#endif
}

/* blob contains exactly argc NUL-terminated UTF-8 strings; argv[0] is
 * the program. No shell is started. Standard streams and environment inherit. */
static int command(const uint8_t* blob, uint64_t length, uint64_t argc, uint64_t* code, int* signal) {
  *code = 0; *signal = 0;
  synx_rt_blocking_check(NULL);
  if (!argc || argc > SIZE_MAX / sizeof(char*) - 1 || !length || length > SIZE_MAX || !blob[0]) return EINVAL;
  uint64_t offset = 0;
  for (uint64_t i = 0; i < argc; ++i) {
    if (offset >= length) return EINVAL;
    const uint8_t* end = (const uint8_t*)memchr(blob + offset, 0, (size_t)(length - offset));
    if (!end) return EINVAL;
    offset = (uint64_t)(end - blob) + 1;
  }
  if (offset != length) return EINVAL;
#if defined(_WIN32)
  wchar_t* args = wide_text(blob, length);
  if (!args) return errno;
  size_t program_length = wcslen(args);
  while (program_length && (args[program_length - 1] == L' ' || args[program_length - 1] == L'.')) --program_length;
  if (program_length >= 4 && (!_wcsnicmp(args + program_length - 4, L".cmd", 4) || !_wcsnicmp(args + program_length - 4, L".bat", 4))) {
    free(args); return EINVAL;
  }
  /* A Windows command line is limited to 32767 UTF-16 units, including NUL.
   * Every argument is quoted using the CRT backslash/quote convention. */
  wchar_t* command = (wchar_t*)malloc(32767 * sizeof(wchar_t));
  if (!command) { free(args); return ENOMEM; }
  size_t used = 0;
  const wchar_t* arg = args;
#define APPEND(c) do { if (used >= 32766) { free(args); free(command); return E2BIG; } command[used++] = (c); } while (0)
  for (uint64_t i = 0; i < argc; ++i) {
    if (i) APPEND(L' ');
    APPEND(L'"');
    const wchar_t* p = arg;
    for (;;) {
      size_t slashes = 0;
      while (*p == L'\\') { ++slashes; ++p; }
      if (!*p || *p == L'"') {
        for (size_t j = 0; j < slashes; ++j) { APPEND(L'\\'); APPEND(L'\\'); }
        if (!*p) break;
        APPEND(L'\\'); APPEND(*p++);
      } else {
        for (size_t j = 0; j < slashes; ++j) APPEND(L'\\');
        APPEND(*p++);
      }
    }
    APPEND(L'"'); arg = p + 1;
  }
#undef APPEND
  command[used] = 0;
  /* Restrict inheritance to private duplicates of the standard streams.
   * Never change the parent's handles or inherit unrelated open files. */
  HANDLE streams[3] = {NULL, NULL, NULL};
  DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  int rc = 0;
  for (int i = 0; i < 3; ++i) {
    HANDLE original = GetStdHandle(ids[i]);
    if (original && original != INVALID_HANDLE_VALUE) {
      if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &streams[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) rc = windows_error(GetLastError());
    } else {
      SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
      streams[i] = CreateFileW(L"NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
      if (streams[i] == INVALID_HANDLE_VALUE) { streams[i] = NULL; rc = windows_error(GetLastError()); }
    }
    if (rc) break;
  }
  STARTUPINFOEXW startup;
  PROCESS_INFORMATION process;
  memset(&process, 0, sizeof process);
  memset(&startup, 0, sizeof startup);
  startup.StartupInfo.cb = sizeof startup;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = streams[0];
  startup.StartupInfo.hStdOutput = streams[1];
  startup.StartupInfo.hStdError = streams[2];
  SIZE_T size = 0;
  BOOL initialized = FALSE;
  if (!rc) {
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(size);
    if (!startup.lpAttributeList) rc = ENOMEM;
    else if (!(initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))) rc = windows_error(GetLastError());
    else if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, streams, sizeof streams, NULL, NULL)) rc = windows_error(GetLastError());
  }
  BOOL ok = FALSE;
  if (!rc) {
    ok = CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &process);
    if (!ok) rc = windows_error(GetLastError());
  }
  if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
  free(startup.lpAttributeList);
  for (int i = 0; i < 3; ++i) if (streams[i]) CloseHandle(streams[i]);
  free(args); free(command);
  if (!ok) return rc;
  DWORD result = 0;
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &result)) rc = windows_error(GetLastError());
  else *code = (uint64_t)result;
  CloseHandle(process.hThread); CloseHandle(process.hProcess); return rc;
#else
  char** argv = (char**)malloc(((size_t)argc + 1) * sizeof(char*));
  if (!argv) return ENOMEM;
  offset = 0;
  for (uint64_t i = 0; i < argc; ++i) { argv[i] = (char*)blob + offset; offset += strlen(argv[i]) + 1; }
  argv[argc] = NULL;
  pid_t pid;
#if defined(__APPLE__)
  int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, *_NSGetEnviron());
#else
  int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
#endif
  free(argv);
  if (rc) return rc;
  int status;
  while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return errno; }
  if (WIFEXITED(status)) *code = (uint64_t)WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) *signal = WTERMSIG(status);
  else return EIO;
  return 0;
#endif
}

/* Keep the C ABI within the four register arguments currently supported by
 * Axon on Windows. Negative wait status encodes a POSIX termination signal. */
/* Both paths must exist. Returns 1 for equal identities, 0 for different
 * identities, or a negative errno. No handle remains open after this call. */
int synx_rt_same_file(const uint8_t* a, uint64_t an, const uint8_t* b, uint64_t bn) {
  int equal = 0;
  int error = same_file(a, an, b, bn, &equal);
  return error ? -error : equal;
}
/* Blocks through child exit. status is the normal unsigned 32-bit exit code
 * or a negative POSIX signal number. The return value is errno, not status.
 * Input storage remains owned by the caller and must survive this call. */
int synx_rt_command(const uint8_t* blob, uint64_t length, uint64_t argc, int64_t* status) {
  uint64_t code = 0;
  int signal = 0;
  int error = command(blob, length, argc, &code, &signal);
  *status = signal ? -(int64_t)signal : (int64_t)code;
  return error;
}
