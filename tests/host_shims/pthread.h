#ifndef PHSTORE_TEST_PTHREAD_H
#define PHSTORE_TEST_PTHREAD_H
#include <windows.h>
typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
static __inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes) { (void)attributes; InitializeSRWLock(mutex); return 0; }
static __inline int pthread_mutex_destroy(pthread_mutex_t *mutex) { (void)mutex; return 0; }
static __inline int pthread_mutex_lock(pthread_mutex_t *mutex) { AcquireSRWLockExclusive(mutex); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t *mutex) { ReleaseSRWLockExclusive(mutex); return 0; }
#endif
