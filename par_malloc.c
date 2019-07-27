#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#include "xmalloc.h"

typedef struct node_t {
  int arena_id;
  struct node_t* next;
  int size; // bytes of free space
} node_t;

typedef struct arena {
    pthread_mutex_t mutex;
    node_t* free_lists[11];
} arena;

__thread arena* local_arena = NULL;
__thread int NUM_ALLOCATIONS = 0;
// to be used in list


//Some type of areana struct that handles the free list

/* Pseudocode for arenas

// at startup:
__thread favorite_arena = 0

int arenas_intialized = 0
for (int i = 0; i < num_threads; i++) {
  initialize_arenas();
}
arenas_intialized = 1;

free_list* arenas[];


// in malloc:
if pthread_try_lock(arenas[favorite_arena]) // if can access:
  pthread_mutex_lock(arena_mutex)
  ... do normal malloc
  pthread_mutex_unlock(arena_mutex)
else:
  arena = arenas[favorite_arena + 1]
  pthread_mutex_lock(arena_mutex)
  ... do normal malloc
  pthread_mutex_unlock(arena_mutex)


// malloc:















*/


/*
bucket[0] = 8
bucket[1] = 16
bucket[2] = 32
bucket[3] = 64
bucket[4] = 128
bucket[5] = 256
bucket[6] = 512
bucket[7] = 1024
bucket[8] = 2048
bucket[9] = 4096
bucket[10] = 8192
*/


// given a multiple of two:
// determine which bucket index this is associated with
// 8 -> 0
// 16 -> 1
// 32 -> 2
// relationship between index and size: 2^(index + 3) = size
// log2(size) - 3
int
get_bucket(long byte){
  double cast = (double) byte;
  double l = log(cast) / log(2.0);
  return ((int) l - 3);
}

arena*
initialize_arena() {

  int scale = 900; // should be divisible by 10

  void* addr = mmap(NULL, 4096 * scale,
    PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);

  void* next_page = addr + 4096 * (scale/11);
  void* max_addr = addr + 4096*scale;

  arena* local_arena = (arena*) addr;
  void* tracked_addr = addr + sizeof(arena);
  pthread_mutex_init(&local_arena->mutex, 0);
  pthread_mutex_lock(&local_arena->mutex);

  // have 4096*15 bytes of memory
  int bucket_index = 0;
  for (int block_size = 8; block_size <= 8192; block_size *=2) {
    node_t* prev = NULL;
    int num_blocks_allocated = 0;
    while (tracked_addr < next_page && tracked_addr < max_addr) {
      node_t* block = (node_t*) tracked_addr;
      block->arena_id = 0;
      block->size = block_size;
      block->next = prev;
      prev = block;
      tracked_addr += sizeof(node_t) + block_size;
      num_blocks_allocated += 1;
    }
    local_arena->free_lists[bucket_index] = prev;
    next_page += 4096 * (scale/11);
    bucket_index += 1;
  }
  pthread_mutex_unlock(&local_arena->mutex);
  return local_arena;
}

int round_down_two(long num) {
  long ii = 8192;

  while (ii > num) {
    ii = ii / 2;
  }
  return ii;
}

long round_up_nearest_page(long num) {
  long init = 4096;
  while (init < num) {
    init += 4096;
  }
  return init;
}

void
place_in_bucket(node_t* block, arena* arena) {
  block->next = arena->free_lists[get_bucket(block->size)];
  arena->free_lists[get_bucket(block->size)] = block;
  return;
}


node_t*
get_bigger_block_for_bucket(long bucket_index) {
  int block_size = pow(2, bucket_index + 4);
  int to_try = bucket_index + 1;

  while (local_arena->free_lists[to_try] == NULL) {
    to_try += 1;
    if (to_try > 9){
      exit(-1);
    }
  }


  node_t* to_return = local_arena->free_lists[to_try];
  local_arena->free_lists[to_try] = to_return->next;

  int remaining_size = to_return->size - sizeof(node_t) - block_size;
  if (remaining_size < 40) {
    to_return->size = block_size;
    to_return->next = NULL;
    return to_return;
  } else {
    to_return->size = block_size;
    to_return->next = NULL;

    void* next = (void*) to_return + sizeof(node_t) + to_return->size;
    node_t* to_insert = (node_t*) next;
    to_insert->size = round_down_two(remaining_size);
    to_insert->next = NULL;
    place_in_bucket(to_insert, local_arena);
    remaining_size -= to_insert->size;

    while (remaining_size >= 40) {
      void* next = (void*) to_insert + sizeof(node_t) + to_insert->size;
      to_insert = (node_t*) next;
      if (to_insert == NULL) {
        break;
      }
      to_insert->size = round_down_two(remaining_size);
      remaining_size -= to_insert->size;
      to_insert->next = NULL;
      place_in_bucket(to_insert, local_arena);
    }

    return to_return;
    }
}

long
round_power_of_two(long num) {
  long ii = 8;

  while (ii < num) {
    ii = ii * 2;
  }
  return ii;
}


void*
xmalloc(size_t bytes)
{
    //printf("Allocating %ld bytes\n", bytes);

    if (local_arena == NULL) {
      local_arena = initialize_arena();
    }
    // small allocations
    long bucket_index = get_bucket(round_power_of_two(bytes));
    if (bucket_index > 10) {
      printf("Big allocation");
      // set header
      void* addr = mmap(NULL, round_up_nearest_page(bytes + 24),
        PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
      node_t* to_return = (node_t*) addr;
      to_return->size = bytes;
      to_return->next = NULL;
      return to_return;
    }
    pthread_mutex_lock(&local_arena->mutex);
    NUM_ALLOCATIONS += 1;
    //printf("pointer for free list for this bucket is %p", local_arena->free_lists[bucket_index]);
    if (local_arena->free_lists[bucket_index] == NULL) {

      node_t* to_return = get_bigger_block_for_bucket(bucket_index);
      pthread_mutex_unlock(&local_arena->mutex);
      return (void*) to_return + sizeof(node_t);
        // either:
        // get block from next highest bucket, split in two, return one, stick one on smaller bucket
        // or, mmap new one (less preferable)
    } else {
      // means that buckets[bucket_index], which is a node_t*, is not NULL
      node_t* to_return = local_arena->free_lists[bucket_index];
      local_arena->free_lists[bucket_index] = to_return->next;
      // move to_return pointer, probably by +16 or +24
      pthread_mutex_unlock(&local_arena->mutex);
      return (void*) to_return + sizeof(node_t);
    }
    printf("Not here");

    return (void*) 0;
}

void
xfree(void* ptr)
{
  void* node = ptr - sizeof(node_t);
  node_t* to_insert = (node_t*) node;
  pthread_mutex_lock(&local_arena->mutex);
  place_in_bucket(to_insert, local_arena);
  pthread_mutex_unlock(&local_arena->mutex);
  return;
}

void*
xrealloc(void* prev, size_t bytes)
{
  int prev_size = *((int*)(prev - 8)); // find size of node to reallocate
  void* new_ptr = xmalloc(bytes); // use our own malloc function to get chunk of new memory of desired size
  memcpy(new_ptr, prev, prev_size); // copy old memory to new memory

  // insert prev back into list
  void* node_ptr = prev - 24; //move header back
  node_t* to_insert = (node_t*) node_ptr; // cast as node_t
  to_insert->size = prev_size - 8;
  to_insert->next = NULL;
  place_in_bucket(to_insert, local_arena); // call insert function

  return new_ptr; // return asked-for pointer
}
