/* OS threads and synchronization. Ownership and containers live in Synx. */
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

extern void* synx_rt_alloc(uint64_t);
extern void synx_rt_free(void*);
extern void synx_rt_panic(const uint8_t*, uint64_t);
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
void (*synx_link_thread_entry)(void*);
#else
extern void synx_thread_entry(void*);
#endif
#if defined(_WIN32)
__declspec(noreturn)
#else
__attribute__((noreturn))
#endif
static void fail(const char* text) { synx_rt_panic((const uint8_t*)text, (uint64_t)strlen(text)); abort(); }
#if defined(_WIN32)
static __declspec(thread) void* current_pool;
static __declspec(thread) int blocking_pool;
#else
static _Thread_local void* current_pool;
static _Thread_local int blocking_pool;
#endif
void synx_rt_pool_enter(void* pool, int32_t blocking) { current_pool = pool; blocking_pool = blocking; }
void synx_rt_pool_leave(void) { current_pool = NULL; blocking_pool = 0; }
void synx_rt_blocking_check(void* pool) {
  if (pool && current_pool == pool) fail("executor cannot block its own worker");
  if (current_pool && !blocking_pool) fail("blocking join on async worker; await the handle");
}
#if defined(_WIN32)
static SRWLOCK input_lock = SRWLOCK_INIT;
void synx_rt_input_lock(void) { AcquireSRWLockExclusive(&input_lock); }
void synx_rt_input_unlock(void) { ReleaseSRWLockExclusive(&input_lock); }
#else
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
void synx_rt_input_lock(void) { if (pthread_mutex_lock(&input_lock)) fail("input lock failed"); }
void synx_rt_input_unlock(void) { if (pthread_mutex_unlock(&input_lock)) fail("input unlock failed"); }
#endif

/* All exposed atomic operations are sequentially consistent, on aligned u64. */
uint64_t synx_rt_atomic_load(uint64_t* p) {
#if defined(_WIN32)
  return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)p, 0, 0);
#else
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}
uint64_t synx_rt_atomic_swap(uint64_t* p, uint64_t value) {
#if defined(_WIN32)
  return (uint64_t)InterlockedExchange64((volatile LONG64*)p, (LONG64)value);
#else
  return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
#endif
}
uint64_t synx_rt_atomic_add(uint64_t* p, uint64_t value) {
#if defined(_WIN32)
  return (uint64_t)InterlockedExchangeAdd64((volatile LONG64*)p, (LONG64)value);
#else
  return __atomic_fetch_add(p, value, __ATOMIC_SEQ_CST);
#endif
}
uint64_t synx_rt_atomic_sub(uint64_t* p, uint64_t value) { return synx_rt_atomic_add(p, 0 - value); }
uint64_t synx_rt_atomic_cas(uint64_t* p, uint64_t expected, uint64_t value) {
#if defined(_WIN32)
  return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)p, (LONG64)value, (LONG64)expected);
#else
  __atomic_compare_exchange_n(p, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return expected;
#endif
}

typedef struct {
#if defined(_WIN32)
  SRWLOCK lock;
#else
  pthread_mutex_t lock;
#endif
} SynxMutex;
void* synx_rt_mutex_new(void) {
  SynxMutex* m = (SynxMutex*)synx_rt_alloc(sizeof(*m));
#if defined(_WIN32)
  InitializeSRWLock(&m->lock);
#else
  if (pthread_mutex_init(&m->lock, NULL)) fail("mutex creation failed");
#endif
  return m;
}
void synx_rt_mutex_lock(SynxMutex* m) {
#if defined(_WIN32)
  AcquireSRWLockExclusive(&m->lock);
#else
  if (pthread_mutex_lock(&m->lock)) fail("mutex lock failed");
#endif
}
int32_t synx_rt_mutex_try_lock(SynxMutex* m) {
#if defined(_WIN32)
  return TryAcquireSRWLockExclusive(&m->lock) ? 1 : 0;
#else
  int code = pthread_mutex_trylock(&m->lock);
  if (code && code != EBUSY) fail("mutex try_lock failed");
  return code == 0;
#endif
}
void synx_rt_mutex_unlock(SynxMutex* m) {
#if defined(_WIN32)
  ReleaseSRWLockExclusive(&m->lock);
#else
  if (pthread_mutex_unlock(&m->lock)) fail("mutex unlock failed");
#endif
}
void synx_rt_mutex_free(SynxMutex* m) {
#if !defined(_WIN32)
  if (pthread_mutex_destroy(&m->lock)) fail("mutex destroyed while locked");
#endif
  synx_rt_free(m);
}

typedef struct {
  uint64_t bound;
#if defined(_WIN32)
  CONDITION_VARIABLE cond;
#else
  pthread_cond_t cond;
#endif
} SynxCondvar;
void* synx_rt_condvar_new(void) {
  SynxCondvar* c = (SynxCondvar*)synx_rt_alloc(sizeof(*c));
  c->bound = 0;
#if defined(_WIN32)
  InitializeConditionVariable(&c->cond);
#else
  pthread_condattr_t attr;
  if (pthread_condattr_init(&attr)) fail("condition variable attributes failed");
#if !defined(__APPLE__)
  if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) fail("condition clock failed");
#endif
  if (pthread_cond_init(&c->cond, &attr)) fail("condition variable creation failed");
  pthread_condattr_destroy(&attr);
#endif
  return c;
}
void synx_rt_condvar_wait(SynxCondvar* c, SynxMutex* m) {
  uint64_t address = (uint64_t)(uintptr_t)m;
  uint64_t previous = synx_rt_atomic_cas(&c->bound, 0, address);
  if (previous && previous != address) fail("condition variable used with different mutexes");
#if defined(_WIN32)
  if (!SleepConditionVariableSRW(&c->cond, &m->lock, INFINITE, 0)) fail("condition wait failed");
#else
  if (pthread_cond_wait(&c->cond, &m->lock)) fail("condition wait failed");
#endif
}
uint64_t synx_rt_monotonic_ms(void) {
#if defined(_WIN32)
  LARGE_INTEGER ticks, frequency;
  if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency)) fail("monotonic clock failed");
  return (uint64_t)(ticks.QuadPart / frequency.QuadPart) * 1000 +
         (uint64_t)((ticks.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart);
#else
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) fail("monotonic clock failed");
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}
/* Return 1 after notification, 0 after timeout. Spurious wakeups are allowed. */
int32_t synx_rt_condvar_wait_for(SynxCondvar* c, SynxMutex* m, uint64_t milliseconds) {
  uint64_t address = (uint64_t)(uintptr_t)m;
  uint64_t previous = synx_rt_atomic_cas(&c->bound, 0, address);
  if (previous && previous != address) fail("condition variable used with different mutexes");
  if (milliseconds > 86400000) milliseconds = 86400000;
#if defined(_WIN32)
  if (SleepConditionVariableSRW(&c->cond, &m->lock, (DWORD)milliseconds, 0)) return 1;
  if (GetLastError() == ERROR_TIMEOUT) return 0;
  fail("condition timed wait failed");
#else
  struct timespec deadline;
#if defined(__APPLE__)
  deadline.tv_sec = (time_t)(milliseconds / 1000);
  deadline.tv_nsec = (long)((milliseconds % 1000) * 1000000);
  int code = pthread_cond_timedwait_relative_np(&c->cond, &m->lock, &deadline);
#else
  if (clock_gettime(CLOCK_MONOTONIC, &deadline)) fail("monotonic clock failed");
  deadline.tv_sec += (time_t)(milliseconds / 1000);
  deadline.tv_nsec += (long)((milliseconds % 1000) * 1000000);
  if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
  int code = pthread_cond_timedwait(&c->cond, &m->lock, &deadline);
#endif
  if (code && code != ETIMEDOUT) fail("condition timed wait failed");
  return code == 0;
#endif
}
void synx_rt_condvar_notify(SynxCondvar* c, int32_t all) {
#if defined(_WIN32)
  if (all) WakeAllConditionVariable(&c->cond); else WakeConditionVariable(&c->cond);
#else
  int code = all ? pthread_cond_broadcast(&c->cond) : pthread_cond_signal(&c->cond);
  if (code) fail("condition notification failed");
#endif
}
void synx_rt_condvar_free(SynxCondvar* c) {
#if !defined(_WIN32)
  if (pthread_cond_destroy(&c->cond)) fail("condition variable destroyed while waiting");
#endif
  synx_rt_free(c);
}

/* A retained notification, also used by synchronous future drivers. */
typedef struct { SynxMutex* lock; SynxCondvar* changed; int ready; } SynxEvent;
void* synx_rt_event_new(void) {
  SynxEvent* e = (SynxEvent*)synx_rt_alloc(sizeof(*e));
  e->lock = (SynxMutex*)synx_rt_mutex_new();
  e->changed = (SynxCondvar*)synx_rt_condvar_new();
  e->ready = 1;
  return e;
}
void synx_rt_event_signal(SynxEvent* e) {
  synx_rt_mutex_lock(e->lock);
  e->ready = 1;
  synx_rt_condvar_notify(e->changed, 1);
  synx_rt_mutex_unlock(e->lock);
}
int32_t synx_rt_event_take(SynxEvent* e) {
  int result;
  synx_rt_mutex_lock(e->lock);
  result = e->ready; e->ready = 0;
  synx_rt_mutex_unlock(e->lock);
  return result;
}
void synx_rt_event_wait(SynxEvent* e) {
  synx_rt_mutex_lock(e->lock);
  while (!e->ready) synx_rt_condvar_wait(e->changed, e->lock);
  synx_rt_mutex_unlock(e->lock);
}
void synx_rt_event_free(SynxEvent* e) {
  synx_rt_condvar_free(e->changed);
  synx_rt_mutex_free(e->lock);
  synx_rt_free(e);
}

typedef struct {
#if defined(_WIN32)
  HANDLE handle;
#else
  pthread_t handle;
#endif
} SynxThread;
#if defined(_WIN32)
static unsigned __stdcall thread_boot(void* job) {
#else
static void* thread_boot(void* job) {
#endif
#if (defined(SYNX_SHARED_RUNTIME) || defined(SYNX_STANDALONE_RUNTIME))
  if (!synx_link_thread_entry) fail("runtime thread entry is not initialized");
  synx_link_thread_entry(job);
#else
  synx_thread_entry(job);
#endif
  synx_rt_free(job);
  return 0;
}
void* synx_rt_thread_start(void* job, int32_t* error) {
  SynxThread* t = (SynxThread*)synx_rt_alloc(sizeof(*t));
#if defined(_WIN32)
  t->handle = (HANDLE)_beginthreadex(NULL, 8 * 1024 * 1024, thread_boot, job, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  *error = t->handle ? 0 : (errno ? errno : EAGAIN);
#else
  pthread_attr_t attr;
  *error = pthread_attr_init(&attr);
  if (!*error) {
    *error = pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    if (!*error) *error = pthread_create(&t->handle, &attr, thread_boot, job);
    pthread_attr_destroy(&attr);
  }
#endif
  if (*error) { synx_rt_free(t); return NULL; }
  return t;
}
void synx_rt_thread_join(SynxThread* t) {
#if defined(_WIN32)
  if (GetThreadId(t->handle) == GetCurrentThreadId()) fail("thread cannot join itself");
  if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) fail("thread join failed");
  CloseHandle(t->handle);
#else
  if (pthread_join(t->handle, NULL)) fail("thread join failed");
#endif
  synx_rt_free(t);
}
void synx_rt_thread_yield(void) {
#if defined(_WIN32)
  SwitchToThread();
#else
  sched_yield();
#endif
}
void synx_rt_thread_sleep(uint64_t milliseconds) {
  /* Bound each OS call so large durations cannot truncate. */
  while (milliseconds) {
    uint64_t chunk = milliseconds > 86400000 ? 86400000 : milliseconds;
#if defined(_WIN32)
    Sleep((DWORD)chunk);
#else
    struct timespec delay;
    delay.tv_sec = (time_t)(chunk / 1000);
    delay.tv_nsec = (long)((chunk % 1000) * 1000000);
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
#endif
    milliseconds -= chunk;
  }
}

#if defined(SYNX_STANDALONE_RUNTIME)
#include "exports_threads.inc"
#endif
