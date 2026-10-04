#include "range_buffer.h"
#include <stdlib.h>
#include <string.h>
int range_buffer_init(range_buffer *b,uint64_t offset,uint64_t size,size_t capacity,range_sink sink,void *ctx){memset(b,0,sizeof(*b));if(!capacity||!sink||!size||offset>UINT64_MAX-size)return -1;b->data=malloc(capacity);if(!b->data)return -1;b->capacity=capacity;b->offset=offset;b->size=size;b->sink=sink;b->ctx=ctx;return 0;}
int range_buffer_flush(range_buffer *b){if(!b->used)return 0;if(b->written>b->size||b->used>b->size-b->written)return -1;if(b->sink(b->ctx,b->data,b->used,b->offset+b->written))return -1;b->written+=b->used;b->used=0;return 0;}
int range_buffer_feed(range_buffer *b,const unsigned char *data,size_t n){if(b->received>b->size||n>b->size-b->received)return -1;while(n){size_t step=b->capacity-b->used;if(step>n)step=n;memcpy(b->data+b->used,data,step);b->used+=step;b->received+=step;data+=step;n-=step;if(b->used==b->capacity&&range_buffer_flush(b))return -1;}return 0;}
void range_buffer_destroy(range_buffer *b){free(b->data);b->data=NULL;b->used=0;}
