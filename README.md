# Optimized-Memory-Allocator
high-performance thread-safe allocator. 


Created with [@SpencerHurley](https://github.com/SpencerHurley) for CS3650 (Computer Systems)



When creating our allocator we decided on using a bucket/arena strategy. We both
thought the concept of dividing the free list into buckets of exact blocks was an
intuitive O(1) solution. We accomplished this by first creating a structure for both
the nodes (chunks) and arenas. We then created the thread accessible variable 
called local_arena that had acesss to its own local freelist. After intializing the 
arena with buckets, we take the amount the user wants to malloc, round it up to
the nearest power of two (to decide what bucket it goes in) and depending on the size
of the allocation to the following. For mallocs that are bigger than the largest bucket
(8129) we just mmap data that is the size of the input + 24 (our header). If it isn't
bigger than the largest bucket, we put the chunk in the correct bucket. We can effenciently
do this by make use of space from larger buckets to accomodate smaller buckets.


