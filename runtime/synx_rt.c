/* Synx runtime: the thin C layer between Synx programs and the operating
 * system. Everything here has scalar parameters only. Struct results are
 * written through an out pointer. The String and Vec layouts match the
 * prelude structs: data pointer, length, capacity, each 8 bytes. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <wchar.h>
#include <windows.h>
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
#include <shellapi.h>
#endif
#define SYNX_WRITE(fd, p, n) _write(fd, p, (unsigned)(n))
#else
#include <unistd.h>
#include <fcntl.h>
#define SYNX_WRITE(fd, p, n) write(fd, p, n)
#endif

typedef struct {
  uint8_t* data;
  uint64_t len;
  uint64_t cap;
} SynxString;

typedef struct {
  uint64_t tag;   /* 0 = None, 1 = Some */
  int32_t value;
} SynxOptionInt;

static void write_all(int fd, const void* p, uint64_t n) {
  const uint8_t* b = (const uint8_t*)p;
  while (n > 0) {
    long w = (long)SYNX_WRITE(fd, b, n);
    if (w <= 0) return;
    b += w;
    n -= (uint64_t)w;
  }
}

void synx_rt_print(const uint8_t* p, uint64_t n) {
#if defined(_WIN32)
  _lock_file(stdout);
#else
  flockfile(stdout);
#endif
  write_all(1, p, n);
  write_all(1, "\n", 1);
#if defined(_WIN32)
  _unlock_file(stdout);
#else
  funlockfile(stdout);
#endif
}

void synx_rt_panic(const uint8_t* p, uint64_t n) {
  write_all(2, "Synx panic: ", 12);
  write_all(2, p, n);
  write_all(2, "\n", 1);
  exit(101);
}

void synx_rt_exit(int32_t code) { exit(code); }

void* synx_rt_alloc(uint64_t size) {
  void* p = malloc(size ? size : 1);
  if (!p) synx_rt_panic((const uint8_t*)"out of memory", 13);
  return p;
}

void* synx_rt_realloc(void* p, uint64_t size) {
  void* q = realloc(p, size ? size : 1);
  if (!q) synx_rt_panic((const uint8_t*)"out of memory", 13);
  return q;
}

void synx_rt_free(void* p) { free(p); }

void synx_rt_memcpy(void* dst, const void* src, uint64_t n) { memcpy(dst, src, n); }

/* Decimal conversion for the compiler lexer. Input is a nonempty ASCII
 * decimal literal without separators or suffix. Output is always written.
 * Returns 0, ERANGE, or EINVAL. Uses the process C locale, as stage 0 does.
 * Subnormal range errors follow the host C library's strtod behavior. */
int32_t synx_rt_parse_double(const uint8_t* text, uint64_t length, double* out) {
  *out = 0.0;
  if (!text || !length || length >= SIZE_MAX) return EINVAL;
  uint64_t i = 0;
  while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
  if (i == 0) return EINVAL;
  if (i < length && text[i] == '.') {
    uint64_t start = ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == start) return EINVAL;
  }
  if (i < length && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
    uint64_t start = i;
    while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == start) return EINVAL;
  }
  if (i != length) return EINVAL;
  char* copy = (char*)synx_rt_alloc(length + 1);
  memcpy(copy, text, (size_t)length);
  copy[length] = '\0';
  errno = 0;
  char* end = NULL;
  *out = strtod(copy, &end);
  int code = errno == ERANGE ? ERANGE : 0;
  if (end != copy + length) code = EINVAL;
  free(copy);
  return code;
}

/* ---------- strings ---------- */

void synx_rt_string_new(SynxString* out) {
  out->data = NULL;
  out->len = 0;
  out->cap = 0;
}

static void string_reserve(SynxString* s, uint64_t extra) {
  if (extra > UINT64_MAX - s->len) {
    const uint8_t message[] = "String capacity is too large";
    synx_rt_panic(message, sizeof(message) - 1);
  }
  uint64_t need = s->len + extra;
  if (need <= s->cap) return;
  uint64_t cap = s->cap ? s->cap : 16;
  while (cap < need) {
    if (cap > UINT64_MAX / 2) { cap = need; break; }
    cap *= 2;
  }
  s->data = (uint8_t*)synx_rt_realloc(s->data, cap);
  s->cap = cap;
}

void synx_rt_string_push_str(SynxString* s, const uint8_t* p, uint64_t n) {
  string_reserve(s, n);
  memcpy(s->data + s->len, p, n);
  s->len += n;
}

void synx_rt_string_drop(SynxString* s) {
  free(s->data);
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}

void synx_rt_vec_drop(SynxString* v) {
  free(v->data);
  v->data = NULL;
  v->len = 0;
  v->cap = 0;
}

void synx_rt_fmt_int(SynxString* s, int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
  synx_rt_string_push_str(s, (const uint8_t*)buf, (uint64_t)n);
}

void synx_rt_fmt_uint(SynxString* s, uint64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
  synx_rt_string_push_str(s, (const uint8_t*)buf, (uint64_t)n);
}

void synx_rt_fmt_double(SynxString* s, double v) {
  char buf[64];
  int n;
  /* NaN and infinity only. Large finite values use the round-trip loop. */
  if (v != v || v > DBL_MAX || v < -DBL_MAX) {
    n = snprintf(buf, sizeof buf, "%g", v);
    synx_rt_string_push_str(s, (const uint8_t*)buf, (uint64_t)n);
    return;
  }
  /* Integral values below 1e15 print as 10.0, not 1e+01. The range test
   * comes first because the cast is undefined outside int64_t. */
  if (v < 1e15 && v > -1e15 && v == (double)(int64_t)v) {
    n = snprintf(buf, sizeof buf, "%.1f", v);
    synx_rt_string_push_str(s, (const uint8_t*)buf, (uint64_t)n);
    return;
  }
  /* Otherwise the shortest form that round trips. */
  for (int prec = 1; prec <= 17; ++prec) {
    n = snprintf(buf, sizeof buf, "%.*g", prec, v);
    if (strtod(buf, NULL) == v) break;
  }
  int has_point = 0;
  for (int i = 0; i < n; ++i) if (buf[i] == '.' || buf[i] == 'e') has_point = 1;
  if (!has_point && n < 60) { buf[n++] = '.'; buf[n++] = '0'; }
  synx_rt_string_push_str(s, (const uint8_t*)buf, (uint64_t)n);
}

void synx_rt_fmt_bool(SynxString* s, uint8_t v) {
  if (v) synx_rt_string_push_str(s, (const uint8_t*)"true", 4);
  else synx_rt_string_push_str(s, (const uint8_t*)"false", 5);
}

void synx_rt_fmt_char(SynxString* s, uint32_t c) {
  uint8_t buf[4];
  int n;
  if (c < 0x80) { buf[0] = (uint8_t)c; n = 1; }
  else if (c < 0x800) { buf[0] = (uint8_t)(0xC0 | (c >> 6)); buf[1] = (uint8_t)(0x80 | (c & 0x3F)); n = 2; }
  else if (c < 0x10000) { buf[0] = (uint8_t)(0xE0 | (c >> 12)); buf[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); buf[2] = (uint8_t)(0x80 | (c & 0x3F)); n = 3; }
  else { buf[0] = (uint8_t)(0xF0 | (c >> 18)); buf[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F)); buf[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (uint8_t)(0x80 | (c & 0x3F)); n = 4; }
  synx_rt_string_push_str(s, buf, (uint64_t)n);
}

int32_t synx_rt_str_cmp(const uint8_t* a, uint64_t an, const uint8_t* b, uint64_t bn) {
  uint64_t n = an < bn ? an : bn;
  int c = memcmp(a, b, n);
  if (c != 0) return c < 0 ? -1 : 1;
  if (an == bn) return 0;
  return an < bn ? -1 : 1;
}

uint64_t synx_rt_str_len(const uint8_t* p, uint64_t n) {
  (void)p;
  return n;
}

void synx_rt_str_parse_int(SynxOptionInt* out, const uint8_t* p, uint64_t n) {
  out->tag = 0;
  out->value = 0;
  if (n == 0) return;
  uint64_t i = 0;
  int neg = 0;
  if (p[0] == '-') { neg = 1; i = 1; }
  else if (p[0] == '+') { i = 1; }
  if (i >= n) return;
  int64_t v = 0;
  for (; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') return;
    v = v * 10 + (p[i] - '0');
    if (v > 2147483648LL) return;
  }
  if (neg) v = -v;
  if (v > 2147483647LL || v < -2147483648LL) return;
  out->tag = 1;
  out->value = (int32_t)v;
}

/* EOF is -1; the out parameter distinguishes EOF from a read failure. */
int synx_rt_input_byte(int* error) {
  fflush(stdout);
  errno = 0;
  int ch = fgetc(stdin);
  *error = ch == EOF && ferror(stdin) ? (errno ? errno : EIO) : 0;
  return ch;
}
static int synx_argc;
static char** synx_argv;
uint64_t synx_rt_arg_count(void) { return (uint64_t)synx_argc; }
const uint8_t* synx_rt_arg_data(uint64_t i) { return i < (uint64_t)synx_argc ? (const uint8_t*)synx_argv[i] : (const uint8_t*)""; }
uint64_t synx_rt_arg_len(uint64_t i) { return i < (uint64_t)synx_argc ? (uint64_t)strlen(synx_argv[i]) : 0; }

/* File operations report errno immediately. Synx owns handles, buffers,
 * retry loops and Result construction. All streams use binary mode. */
static int io_error(void) { return errno ? errno : EIO; }
#if defined(_WIN32)
static wchar_t* file_path(const uint8_t* p, uint64_t n) {
  if (n > INT_MAX || memchr(p, 0, (size_t)n)) { errno = EINVAL; return NULL; }
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, NULL, 0);
  if (!count) { errno = EINVAL; return NULL; }
  wchar_t* result = (wchar_t*)malloc(((size_t)count + 1) * sizeof(wchar_t));
  if (!result) { errno = ENOMEM; return NULL; }
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, result, count);
  result[count] = 0;
  return result;
}
#else
static char* file_path(const uint8_t* p, uint64_t n) {
  if (n >= SIZE_MAX || memchr(p, 0, (size_t)n)) { errno = EINVAL; return NULL; }
  char* result = (char*)malloc((size_t)n + 1);
  if (!result) { errno = ENOMEM; return NULL; }
  memcpy(result, p, (size_t)n); result[n] = 0;
  return result;
}
#endif
int synx_rt_file_open(const uint8_t* p, uint64_t n, int mode, FILE** out) {
  *out = NULL;
  if (mode < 0 || mode > 3) return EINVAL;
  if (!n) return ENOENT;
#if defined(_WIN32)
  wchar_t* path = file_path(p, n);
  if (!path) return io_error();
  FILE* f = _wfopen(path, mode == 0 ? L"rbN" : mode == 1 ? L"wbN" : mode == 2 ? L"abN" : L"r+bN");
#else
  char* path = file_path(p, n);
  if (!path) return io_error();
  int flags = mode == 0 ? O_RDONLY : mode == 1 ? O_WRONLY | O_CREAT | O_TRUNC : mode == 2 ? O_WRONLY | O_CREAT | O_APPEND : O_RDWR;
  int fd = open(path, flags | O_CLOEXEC, 0666);
  FILE* f = fd < 0 ? NULL : fdopen(fd, mode == 0 ? "rb" : mode == 1 ? "wb" : mode == 2 ? "ab" : "r+b");
  if (fd >= 0 && !f) { int error = io_error(); close(fd); errno = error; }
#endif
  int error = f ? 0 : io_error(); free(path); *out = f; return error;
}
int synx_rt_file_close(FILE* f) { return fclose(f) == 0 ? 0 : io_error(); }
int synx_rt_file_flush(FILE* f) { return fflush(f) == 0 ? 0 : io_error(); }
int synx_rt_file_read(FILE* f, uint8_t* p, uint64_t n, uint64_t* count) {
  errno = 0;
  *count = (uint64_t)fread(p, 1, (size_t)n, f);
  return ferror(f) ? io_error() : 0;
}
int synx_rt_file_write(FILE* f, const uint8_t* p, uint64_t n, uint64_t* count) {
  errno = 0;
  *count = (uint64_t)fwrite(p, 1, (size_t)n, f);
  return *count == n ? 0 : io_error();
}
int synx_rt_file_seek(FILE* f, int64_t offset, int origin) {
  int base = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
  if (origin < 0 || origin > 2) return EINVAL;
#if defined(_WIN32)
  if (_fseeki64(f, offset, base)) return io_error();
#else
  if (fseeko(f, (off_t)offset, base)) return io_error();
#endif
  return 0;
}
int synx_rt_file_position(FILE* f, uint64_t* out) {
#if defined(_WIN32)
  int64_t position = _ftelli64(f);
#else
  int64_t position = (int64_t)ftello(f);
#endif
  if (position < 0) return io_error();
  *out = (uint64_t)position; return 0;
}
int synx_rt_file_remove(const uint8_t* p, uint64_t n) {
  if (!n) return ENOENT;
#if defined(_WIN32)
  wchar_t* path = file_path(p, n);
  if (!path) return io_error();
  int rc = _wremove(path);
#else
  char* path = file_path(p, n);
  if (!path) return io_error();
  int rc = remove(path);
#endif
  int error = rc ? io_error() : 0; free(path); return error;
}
int synx_rt_io_kind(int code) {
  if (code == ENOENT || code == ENOTDIR) return 1;
  if (code == EACCES || code == EPERM) return 2;
  if (code == EINVAL) return 3;
  if (code == EEXIST) return 4;
  if (code == EILSEQ) return 5;
  return 0;
}
int synx_rt_windows(void) {
#if defined(_WIN32)
  return 1;
#else
  return 0;
#endif
}

int synx_rt_file_metadata(const uint8_t* p, uint64_t n, int* kind, uint64_t* size) {
  if (!n) return ENOENT;
#if defined(_WIN32)
  /* Win32 directly, following links. CRT _wstat64 maps some failures
     differently across C runtime versions (a link that cannot be resolved
     reads as "not found" in one and succeeds in another). The not-found set
     matches the MSVC std::filesystem implementation. */
  wchar_t* path = file_path(p, n);
  if (!path) return io_error();
  HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(path);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_BAD_NETPATH || code == ERROR_INVALID_NAME) return ENOENT;
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) return EACCES;
    return EIO;
  }
  FILE_BASIC_INFO basic; FILE_STANDARD_INFO standard;
  int error = 0;
  if (GetFileType(handle) != FILE_TYPE_DISK) { *kind = 3; *size = 0; }
  else if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof basic) ||
           !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof standard)) error = EIO;
  else {
    *kind = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
    *size = (uint64_t)standard.EndOfFile.QuadPart;
  }
  CloseHandle(handle);
  return error;
#else
  char* path = file_path(p, n);
  if (!path) return io_error();
  struct stat info;
  int rc = stat(path, &info);
  int error = rc ? io_error() : 0;
  if (!rc) { *kind = S_ISDIR(info.st_mode) ? 2 : S_ISREG(info.st_mode) ? 1 : 3; }
  if (!rc) *size = (uint64_t)info.st_size;
  free(path); return error;
#endif
}
int synx_rt_create_dir(const uint8_t* p, uint64_t n) {
  if (!n) return ENOENT;
#if defined(_WIN32)
  wchar_t* path = file_path(p, n);
  if (!path) return io_error();
  int rc = _wmkdir(path);
#else
  char* path = file_path(p, n);
  if (!path) return io_error();
  int rc = mkdir(path, 0777);
#endif
  int error = rc ? io_error() : 0; free(path); return error;
}
int synx_rt_remove_dir(const uint8_t* p, uint64_t n) {
  if (!n) return ENOENT;
#if defined(_WIN32)
  wchar_t* path = file_path(p, n);
  if (!path) return io_error();
  int rc = _wrmdir(path);
#else
  char* path = file_path(p, n);
  if (!path) return io_error();
  int rc = rmdir(path);
#endif
  int error = rc ? io_error() : 0; free(path); return error;
}
int synx_rt_file_rename(const uint8_t* a, uint64_t an, const uint8_t* b, uint64_t bn) {
  if (!an || !bn) return ENOENT;
#if defined(_WIN32)
  wchar_t* from = file_path(a, an);
  if (!from) return io_error();
  wchar_t* to = file_path(b, bn);
  if (!to) { int error = io_error(); free(from); return error; }
  int rc = _wrename(from, to);
#else
  char* from = file_path(a, an);
  if (!from) return io_error();
  char* to = file_path(b, bn);
  if (!to) { int error = io_error(); free(from); return error; }
  int rc = rename(from, to);
#endif
  int error = rc ? io_error() : 0; free(from); free(to); return error;
}
int synx_rt_current_dir(uint8_t* buffer, uint64_t cap, uint64_t* count) {
#if defined(_WIN32)
  wchar_t* path = _wgetcwd(NULL, 0);
  if (!path) return io_error();
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
  if (!n) { free(path); return EINVAL; }
  *count = (uint64_t)n - 1;
  if (cap < *count) { free(path); return -1; }
  /* Conversion includes a terminating NUL; use a temporary with room for it. */
  char* bytes = (char*)malloc((size_t)n);
  if (!bytes) { free(path); return ENOMEM; }
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, bytes, n, NULL, NULL);
  memcpy(buffer, bytes, (size_t)*count); free(bytes); free(path);
#else
  errno = 0;
  char* path = getcwd((char*)buffer, (size_t)cap);
  if (!path) {
    if (errno == ERANGE) { *count = cap <= UINT64_MAX / 2 ? cap * 2 : UINT64_MAX; return -1; }
    return io_error();
  }
  *count = (uint64_t)strlen(path);
#endif
  return 0;
}

/* ---------- entry ---------- */

#if !(defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
int synx_entry(void);
#endif

#if defined(_WIN32)
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
extern void (*synx_link_thread_entry)(void*);
int synx_rt_link_start_v1(int (*entry)(void), void (*thread_entry)(void*)) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) synx_rt_panic((const uint8_t*)"cannot decode command line", 26);
  synx_link_thread_entry = thread_entry;
#else
int wmain(int argc, wchar_t** argv) {
#endif
  _setmode(_fileno(stdin), _O_BINARY);
  synx_argv = (char**)synx_rt_alloc((uint64_t)argc * sizeof(char*));
  for (int i = 0; i < argc; ++i) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, NULL, 0, NULL, NULL);
    if (!n) { const uint8_t message[] = "command line is not valid Unicode"; synx_rt_panic(message, sizeof(message) - 1); }
    synx_argv[i] = (char*)synx_rt_alloc((uint64_t)n);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, synx_argv[i], n, NULL, NULL);
  }
#else
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
extern void (*synx_link_thread_entry)(void*);
int synx_rt_link_start_v1(int argc, char** argv, int (*entry)(void), void (*thread_entry)(void*)) {
  synx_link_thread_entry = thread_entry;
#else
int main(int argc, char** argv) {
#endif
  synx_argv = argv;
#endif
  synx_argc = argc;
  int code =
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
      entry();
#else
      synx_entry();
#endif
  fflush(stdout);
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME)) && defined(_WIN32)
  /* Preserve process arguments until process exit; callbacks may still read them. */
  LocalFree(argv);
  exit(code);
#else
  return code;
#endif
}

#if defined(SYNX_STANDALONE_RUNTIME)
#include "exports_rt.inc"
#endif
