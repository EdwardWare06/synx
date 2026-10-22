/* Compiler CLI transport. No parsing, semantic checks or code generation. */
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

/* TargetOs ordinal: Linux=0, Windows=1, MacOs=2; 3 is unsupported. */
int synx_rt_driver_host(void) {
#if defined(_WIN32)
  return 1;
#elif defined(__APPLE__)
  return 2;
#elif defined(__linux__)
  return 0;
#else
  return 3;
#endif
}

/* Match the bootstrap CLI's CRT text streams. Child programs configure their
 * own streams in the ordinary runtime entry point. */
void synx_rt_driver_init(void) {
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_TEXT);
  _setmode(_fileno(stderr), _O_TEXT);
#endif
}

/* Writes exactly n bytes before CRT newline conversion. Flush before spawning
 * children so verbose compiler output precedes inherited child output. */
int synx_rt_driver_write(int fd, const uint8_t* data, uint64_t n) {
  FILE* stream = fd == 1 ? stdout : fd == 2 ? stderr : NULL;
  if (!stream || (!data && n)) return EINVAL;
  while (n) {
    size_t chunk = n > UINT_MAX ? UINT_MAX : (size_t)n;
    size_t written = fwrite(data, 1, chunk, stream);
    if (!written) return errno ? errno : EIO;
    data += written; n -= written;
  }
  return fflush(stream) == 0 ? 0 : (errno ? errno : EIO);
}

/* C++ ostringstream's default floating format uses six significant digits.
 * Decimal conversion/formatting remains a native boundary in this port. */
uint64_t synx_rt_driver_float(double value, uint8_t* output, uint64_t capacity) {
  if (!output || capacity < 64) return 0;
  int count = snprintf((char*)output, 64, "%.6g", value);
  return count > 0 && count < 64 ? (uint64_t)count : 0;
}
