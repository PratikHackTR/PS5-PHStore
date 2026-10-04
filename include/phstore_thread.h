#ifndef PHSTORE_THREAD_H
#define PHSTORE_THREAD_H

#include <pthread.h>

/* Includes headroom for nested HTTP parsing/source reads and native status.
 * Do not inherit an unknown payload-loader pthread default for these workers. */
#define PHSTORE_WORKER_STACK_BYTES (512u * 1024u)
static inline int phstore_thread_create(pthread_t *thread,
                                       void *(*entry)(void *), void *argument) {
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0) return result;
    result = pthread_attr_setstacksize(&attributes, PHSTORE_WORKER_STACK_BYTES);
    if (result == 0) result = pthread_create(thread, &attributes, entry, argument);
    (void)pthread_attr_destroy(&attributes);
    return result;
}
#endif
