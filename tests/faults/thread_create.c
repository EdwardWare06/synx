/* Linux test interposer: exercise spawn cleanup after OS creation fails. */
#include <pthread.h>
#include <errno.h>
int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* arg) {
    (void)thread; (void)attr; (void)entry; (void)arg;
    return EAGAIN;
}
