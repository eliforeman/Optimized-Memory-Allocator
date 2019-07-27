
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "hmalloc.h"

// to be used in list
typedef struct node_t {
  struct node_t* next;
  int size; // bytes of free space
} node_t;

// represents a block of allocated space
typedef struct header_t {
  size_t size; // amount of space allocated
} header_t;

const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.
node_t* free_list = NULL;
pthread_mutex_t mutex;
int mutex_initialized = 0;

// coalesce the free list so that adjacent nodes are converted into single nodes
void coalesce_free_list() {
  pthread_mutex_lock(&mutex);
  node_t* candidate = free_list;
  if (candidate == NULL || candidate->next == NULL) {
    pthread_mutex_unlock(&mutex);
    return;
  }

  while (candidate != NULL) {
    int diff = (void*)candidate->next - (void*)candidate;
    if (diff == candidate->size + sizeof(node_t)) {
      node_t* to_merge = candidate->next;
      candidate->size += to_merge->size + sizeof(node_t);
      candidate->next = to_merge->next;
    } else {
      candidate = candidate->next;
    }
  }
  pthread_mutex_unlock(&mutex);
  return;
}

void
insert_node(node_t* to_insert) {
  pthread_mutex_lock(&mutex);
  node_t* next_ptr = free_list;
  node_t* prev = NULL;
  while (next_ptr != NULL) {
    if (to_insert < next_ptr) {
      if (prev == NULL) {
        free_list = to_insert;
        to_insert->next = next_ptr;
        break;
      } else {
        to_insert->next = prev->next;
        prev->next = to_insert; //TODO might cause segfault
        break;
      }
    } else {
      prev = next_ptr;
      next_ptr = next_ptr->next;
    }
  }
  if (prev == NULL) {
    free_list = to_insert;
  } else {
    prev->next = to_insert;
  }
  pthread_mutex_unlock(&mutex);
  coalesce_free_list();
}


// Find the node_t with enough space to accomodate this allocation
// and remove it from the free list.
// if such a cell doesn't exist, return NULL
node_t* find_cell_for_space(int space_required) {
  pthread_mutex_lock(&mutex);
  node_t* candidate = free_list;
  node_t* prev_node = NULL;
  while (candidate != NULL) {
    if (candidate->size > space_required) {
      if (prev_node == NULL) { // node we popped was at front of list
        free_list = candidate->next;
      } else { // node we popped was not at front of list
        prev_node->next = candidate->next;
      }
      pthread_mutex_unlock(&mutex);
      return candidate;
    } else {
      candidate = candidate->next;
    }
  }
  pthread_mutex_unlock(&mutex);
  return NULL;
}

long
free_list_length()
{
    coalesce_free_list();
    pthread_mutex_lock(&mutex);
    node_t* next_cell = free_list;
    int length = 0;
    while (next_cell != NULL) {
      length += 1;
      next_cell = next_cell->next;
    }
    pthread_mutex_unlock(&mutex);
    return length;
}

hm_stats*
hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats()
{
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}

void*
hmalloc(size_t size)
{
    stats.chunks_allocated += 1;
    size += sizeof(size_t);
    if (mutex_initialized == 0) {
      pthread_mutex_init(&mutex, 0);
      mutex_initialized = 1;
    }
    if (size < PAGE_SIZE) {
      // see if there's a big enough block on the free list
      node_t* maybe_block = find_cell_for_space(size);
      if (maybe_block != NULL) {

        header_t* to_return = (header_t*) maybe_block;
        to_return->size = size - 8;
        void* mem_ptr = (void*) to_return + 8;

        size_t remaining_size = maybe_block->size - size; // used to set size of next block
        if (remaining_size > sizeof(node_t)) {
          // insert remaining memory back into free list
          void* next_ptr = mem_ptr + to_return->size;
          node_t* to_insert = (node_t*) next_ptr;
          to_insert->size = remaining_size; // TODO probably - size + sizeof(node_t)
          to_insert->next = NULL;
          insert_node(to_insert);
        }

        return mem_ptr;
      } else {
        // else if you don't have a block, mmap a new block (1 page = 4096 bytes)
        void* addr = mmap(NULL, 4096,
          PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
        stats.pages_mapped += 1;
        // if the block is bigger than the request, and the leftover is big enough
        // to store a free list cell, return the extra to the free list
        // use the start of the block to store its size
        if (4096 - size > sizeof(node_t)) {
          header_t* block = (header_t*) addr;
          block->size = size - sizeof(size_t);
          void* mem_ptr = block + 1; // pointer to (size) bytes of memory

          void* next_ptr = mem_ptr + block->size;
          node_t* to_insert = (node_t*) next_ptr;
          to_insert->size = 4096 - size - sizeof(node_t);
          to_insert->next = NULL;

          //return extra to the free list
          insert_node(to_insert);

          return mem_ptr;
        } else { // just return the block
          header_t* block = (header_t*) addr;
          block->size = size - sizeof(size_t);
          void* mem_ptr = block + 1;
          return mem_ptr;
        }

      }

      // return a pointer to the block AFTER the size field

    } else {
      // calculate the number of pages needed for this block
      size_t num_pages = div_up(size, PAGE_SIZE);
      // allocate that many pages with mmap
      // fill in the size of the block as # of pages * 4096
      void* addr = mmap(NULL, num_pages * 4096,
        PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
      stats.pages_mapped += num_pages;

      header_t* header = (header_t*) addr;
      header->size = num_pages * 4096;
      header += 1; // set pointer to after the size field
      // return a pointer to the block after the size field
      return (void*) header;

    }

    return (void*) 0xDEADBEEF;
}



void
hfree(void* item)
{
    stats.chunks_freed += 1;
    item -= sizeof(size_t);
    header_t* block = (header_t*) item;
    if (block->size < PAGE_SIZE) {
      size_t size = block->size - 8;
      //if the block is < 1 page then stick it on the free list.
      node_t* to_insert = (node_t*) block;
      to_insert->size = size;
      to_insert->next = NULL;

      insert_node(to_insert);
    } else {
      //If the block is >= 1 page, then munmap it.
      size_t num_pages = div_up(block->size, PAGE_SIZE);
      stats.pages_unmapped += num_pages;
      munmap(block, block->size);
    }
}

void*
hrealloc(void* prev, size_t bytes) {
  int prev_size = *((int*)(prev - 8)); // find size of node to reallocate
  void* new_ptr = hmalloc(bytes); // use our own malloc function to get chunk of new memory of desired size
  memcpy(new_ptr, prev, prev_size); // copy old memory to new memory

  // insert prev back into list
  void* node_ptr = prev - 8; //move header back
  node_t* to_insert = (node_t*) node_ptr; // cast as node_t
  to_insert->size = prev_size - 8;
  to_insert->next = NULL;
  insert_node(to_insert); // call insert function

  return new_ptr; // return asked-for pointer
}
