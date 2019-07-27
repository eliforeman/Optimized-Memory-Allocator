#ifndef HMALLOC_H
#define HMALLOC_H

void hfree(void* item);
void* hrealloc(void* prev, size_t bytes);
void* hmalloc(size_t size);

#endif
