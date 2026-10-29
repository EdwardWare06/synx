/* Host file publication for the built-in linker. All compiler bytes and symbol
 * processing remain in Synx. Execute permission is applied without a shell. */
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
static volatile LONG publication_counter;
static wchar_t* publication_target(const wchar_t* path) {
  DWORD length = GetFullPathNameW(path, 0, NULL, NULL);
  if (!length) return NULL;
  wchar_t* absolute = (wchar_t*)malloc(((size_t)length + 8) * sizeof(wchar_t));
  if (!absolute) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  DWORD written = GetFullPathNameW(path, length, absolute, NULL);
  if (!written || written >= length) { free(absolute); return NULL; }
  for (DWORD i = 0; i < written; ++i) if (absolute[i] == L'/') absolute[i] = L'\\';
  if (wcsncmp(absolute, L"\\\\?\\", 4) == 0) return absolute;
  wchar_t* extended = (wchar_t*)malloc(((size_t)written + 9) * sizeof(wchar_t));
  if (!extended) { free(absolute); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  if (wcsncmp(absolute, L"\\\\", 2) == 0) {
    wcscpy_s(extended, (size_t)written + 9, L"\\\\?\\UNC\\");
    wcscat_s(extended, (size_t)written + 9, absolute + 2);
  } else {
    wcscpy_s(extended, (size_t)written + 9, L"\\\\?\\");
    wcscat_s(extended, (size_t)written + 9, absolute);
  }
  free(absolute); return extended;
}
#endif
static int publication_write(const uint8_t* path, uint64_t length, const uint8_t* bytes, uint64_t size, int executable) {
  if (!length || length >= INT_MAX || memchr(path, 0, (size_t)length)) return EINVAL;
#if defined(_WIN32)
  (void)executable;
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)path, (int)length, NULL, 0);
  if (!count) return EINVAL;
  wchar_t* decoded = (wchar_t*)malloc(((size_t)count + 1) * sizeof(wchar_t));
  if (!decoded) return ENOMEM;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)path, (int)length, decoded, count); decoded[count] = 0;
  wchar_t* target = publication_target(decoded); free(decoded);
  if (!target) return (int)GetLastError();
  size_t capacity = wcslen(target) + 80;
  wchar_t* temporary = (wchar_t*)malloc(capacity * sizeof(wchar_t));
  if (!temporary) { free(target); return ENOMEM; }
  const wchar_t* slash = wcsrchr(target, L'\\');
  size_t directory_length = slash ? (size_t)(slash - target + 1) : 0;
  memcpy(temporary, target, directory_length * sizeof(wchar_t));
  HANDLE file = INVALID_HANDLE_VALUE; int error = 0;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    unsigned counter = (unsigned)InterlockedIncrement(&publication_counter);
    swprintf_s(temporary + directory_length, capacity - directory_length,
        L".synx-%lu-%llu-%u.tmp", GetCurrentProcessId(), (unsigned long long)GetTickCount64(), counter);
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) break;
    error = (int)GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
  }
  if (file == INVALID_HANDLE_VALUE) { free(temporary); free(target); return error; }
  error = 0;
  while (!error && size) {
    DWORD chunk = size > 0x7fffffff ? 0x7fffffff : (DWORD)size; DWORD written = 0;
    if (!WriteFile(file, bytes, chunk, &written, NULL)) { error = (int)GetLastError(); break; }
    if (!written) { error = ERROR_WRITE_FAULT; break; }
    bytes += written; size -= written;
  }
  if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) error = (int)GetLastError();
  if (!error) {
    for (unsigned retry = 0; ; ++retry) {
      if (MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
      error = (int)GetLastError();
      if (retry == 10 || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)) break;
      Sleep(25); error = 0;
    }
  }
  if (error) DeleteFileW(temporary);
  free(temporary); free(target); return error;
#else
  if (length > SIZE_MAX - 16) return EINVAL;
  char* target = (char*)malloc((size_t)length + 1);
  char* temporary = (char*)malloc((size_t)length + 16);
  if (!target || !temporary) { free(target); free(temporary); return ENOMEM; }
  memcpy(target, path, (size_t)length); target[length] = 0;
  memcpy(temporary, target, (size_t)length); memcpy(temporary + length, ".synx-XXXXXX", 13);
  int file = mkstemp(temporary); int error = file < 0 ? errno : 0;
  while (!error && size) {
    size_t chunk = size > INT_MAX ? INT_MAX : (size_t)size;
    ssize_t written = write(file, bytes, chunk);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { error = written < 0 ? errno : EIO; break; }
    bytes += written; size -= (uint64_t)written;
  }
  /* mkstemp applies 0600. Executables get 0777 minus the umask, as a system
     linker would create them. The compiler links on one thread, so reading
     the umask by setting and restoring it is safe here. */
  mode_t mask = umask(022); umask(mask);
  if (!error && fchmod(file, (executable ? 0777 : 0666) & ~mask)) error = errno;
  if (!error && fsync(file)) error = errno;
  if (file >= 0 && close(file) && !error) error = errno;
  if (!error && rename(temporary, target)) error = errno;
  if (error && file >= 0) unlink(temporary);
  free(target); free(temporary); return error;
#endif
}
int synx_rt_link_write(const uint8_t* path, uint64_t length, const uint8_t* bytes, uint64_t size) {
  return publication_write(path, length, bytes, size, 1);
}
/* Atomic manifest/lock publication uses ordinary data-file permissions. */
int synx_rt_myelin_write(const uint8_t* path, uint64_t length, const uint8_t* bytes, uint64_t size) {
  return publication_write(path, length, bytes, size, 0);
}
int synx_rt_link_executable(const uint8_t* path, uint64_t length) {
#if defined(_WIN32)
  (void)path; (void)length; return 0;
#else
  if (length >= SIZE_MAX || memchr(path, 0, (size_t)length)) return EINVAL;
  char* text = (char*)malloc((size_t)length + 1);
  if (!text) return ENOMEM;
  memcpy(text, path, (size_t)length); text[length] = 0;
  mode_t mask = umask(022); umask(mask);
  int status = chmod(text, 0777 & ~mask); int error = status ? errno : 0;
  free(text); return error;
#endif
}

#if defined(SYNX_STANDALONE_RUNTIME)
#include "exports_link.inc"
#endif
