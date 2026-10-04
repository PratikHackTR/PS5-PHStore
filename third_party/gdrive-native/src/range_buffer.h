#ifndef DEMO_RANGE_BUFFER_H
#define DEMO_RANGE_BUFFER_H
#include <stddef.h>
#include <stdint.h>
typedef int (*range_sink)(void *,const unsigned char *,size_t,uint64_t);
typedef struct {unsigned char *data;size_t capacity,used;uint64_t offset,size,received,written;range_sink sink;void *ctx;} range_buffer;
int range_buffer_init(range_buffer *,uint64_t,uint64_t,size_t,range_sink,void *);
int range_buffer_feed(range_buffer *,const unsigned char *,size_t);
int range_buffer_flush(range_buffer *);
void range_buffer_destroy(range_buffer *);
#endif
