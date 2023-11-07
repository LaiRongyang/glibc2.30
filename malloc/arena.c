/* 无锁争用的多线程Malloc实现. */

#include <stdbool.h>

/* Compile-time constants.  */
#define HEAP_MIN_SIZE (32 * 1024) //32 KB   /*HEAP_MIN_SIZE 是heap 的对齐*/
#ifndef HEAP_MAX_SIZE 
# ifdef DEFAULT_MMAP_THRESHOLD_MAX // 64位机是4MB
#  define HEAP_MAX_SIZE (2 * DEFAULT_MMAP_THRESHOLD_MAX) //64 MB 64位机
# else
#  define HEAP_MAX_SIZE (1024 * 1024) /* must be a power of two */
# endif
#endif

/* HEAP_MIN_SIZE and HEAP_MAX_SIZE limit the size of mmap()ed heaps
   that are dynamically created for multi-threaded programs. The
   maximum size must be a power of two, for fast determination of
   which heap belongs to a chunk.  It should be much larger than the
   mmap threshold, so that requests with a size just below that
   threshold can be fulfilled without creating too many heaps.  */

/***************************************************************************/

#define top(ar_ptr) ((ar_ptr)->top)

/* A heap is a single contiguous memory region holding (coalesceable)
   malloc_chunks.  It is allocated with mmap() and always starts at an
   address aligned to HEAP_MAX_SIZE.  */


typedef struct _heap_info
{
  mstate ar_ptr; /* 指向所属分配区的指针. */    //arena 又是怎么定义
  struct _heap_info *prev; /* Previous heap.用于将同一个分配区中的 sub_heap 用单向链表链接起来  prev 指向链表中的前一个 sub_heap*/ 
  size_t size;   /* Current size in bytes. 表示当前 sub_heap 中的内存大小，以 page 对齐*/       
  size_t mprotect_size; /* Size in bytes that has been mprotected
                           PROT_READ|PROT_WRITE. 当前 sub_heap 中被读写保护的内存大小，被分配的还是没被分配的？ */
  /* Make sure the following data is properly aligned, particularly
     that sizeof (heap_info) + 2 * SIZE_SZ is a multiple of
     MALLOC_ALIGNMENT. 段用于保证 sizeof (heap_info) + 2 * SIZE_SZ 是按 MALLOC_ALIGNMENT 对齐的 */
  char pad[-6 * SIZE_SZ & MALLOC_ALIGN_MASK];
} heap_info;


/* Thread specific data.  */
/* 线程私有的分配区实例 */
/*  __thread是gcc内置的线程局部存储设施，存取效率可以和全局变量相比 */
static __thread mstate thread_arena attribute_tls_model_ie;   


/* Arena free list.  free_list_lock synchronizes access to the
   free_list variable below, and the next_free and attached_threads
   members of struct malloc_state objects.  No other locks must be
   acquired after free_list_lock has been acquired.  */
/* __libc_lock_define_initialized 是 glibc（GNU C Library）中的一个宏定义 这个宏的目的是定义一个静态的互斥锁，并且在定义时就进行了初始化。*/

__libc_lock_define_initialized (static, free_list_lock);
static size_t narenas = 1;
static mstate free_list;   

/* list_lock prevents concurrent（同时的） writes to the next member of struct
   malloc_state objects.

   Read access to the next member is supposed to synchronize with the
   atomic_write_barrier and the write to the next member in
   _int_new_arena.  This suffers from data races; see the FIXME
   comments in _int_new_arena and reused_arena.

   list_lock also prevents concurrent forks.  At the time list_lock is
   acquired, no arena lock must have been acquired, but it is
   permitted to acquire arena locks subsequently, while list_lock is
   acquired.  */
__libc_lock_define_initialized (static, list_lock);

/* Already initialized? */
int __malloc_initialized = -1;

/**************************************************************************/


/* arena_get() acquires an arena and locks the corresponding mutex.
   First, try the one last locked successfully by this thread.  (This
   is the common case and handled with a macro for speed.)  Then, loop
   once over the circularly linked list of arenas.  If no arena is
   readily available, create a new one.  In this latter case, `size'
   is just a hint as to how much memory will be required immediately
   in the new arena. */

/* 宏尝试查看线程的私用实例中是否包含一个分配区，如果存在则加锁返回，如果不存在分配区就会调用 arena_get2()函数获得一个分配区 */
#define arena_get(ptr, size) do { \
      ptr = thread_arena;						      \
      arena_lock (ptr, size);						      \
  } while (0)

/* 如果存在分配区指针 则加锁 ，否则调用 arena_get2 获得一个分配区指针。 */
#define arena_lock(ptr, size) do {					      \
      if (ptr)								      \
        __libc_lock_lock (ptr->mutex);					      \
      else		
      /* 获得分配区指针*/						      \
        ptr = arena_get2 ((size), NULL);				      \
  } while (0)

/* find the heap and corresponding arena for a given ptr */
/* 每个 sub_heap 的内存块使用 mmap()函数分配，并以 HEAP_MAX_SIZE 对齐，所以可以根据 chunk 的指针地址，获得这个 chunk 所属的 sub_heap 的地址 */
#define heap_for_ptr(ptr)  ((heap_info *) ((unsigned long) (ptr) & ~(HEAP_MAX_SIZE - 1))) 
/* 如果不属于主分配区，由于 sub_heap 的头部存放的是 heap_info 的实例，heap_info中保存了分配区的指针，所以可以通过 chunk 的地址获得分配区的地址*/
#define arena_for_chunk(ptr) (chunk_main_arena (ptr) ? &main_arena : heap_for_ptr (ptr)->ar_ptr)


/**************************************************************************/

/* atfork support.  */

/* The following three functions are called around fork from a
   multi-threaded process.  We do not use the general fork handler
   mechanism to make sure that our handlers are the last ones being
   called, so that other fork handlers can use the malloc
   subsystem.  */

/* 对非free分配区list加锁 */
void __malloc_fork_lock_parent (void)
{
  if (__malloc_initialized < 1)
    return;

  /* We do not acquire free_list_lock here because we completely
     reconstruct free_list in __malloc_fork_unlock_child.  */

  __libc_lock_lock (list_lock);

  for (mstate ar_ptr = &main_arena;; )
  {
    __libc_lock_lock (ar_ptr->mutex);
    ar_ptr = ar_ptr->next;
    if (ar_ptr == &main_arena)
      break;
  }
}
/* 对非free分配区list解锁 */
void __malloc_fork_unlock_parent (void)
{
  if (__malloc_initialized < 1)
    return;

  for (mstate ar_ptr = &main_arena;; )
    {
      __libc_lock_unlock (ar_ptr->mutex);
      ar_ptr = ar_ptr->next;
      if (ar_ptr == &main_arena)
        break;
    }
  __libc_lock_unlock (list_lock);
}

void __malloc_fork_unlock_child (void)
{
  if (__malloc_initialized < 1)
    return;

  /* 将所有arena推到空闲列表中，除了附加到当前线程的thread_arena。  */
  __libc_lock_init (free_list_lock);
  if (thread_arena != NULL)
    thread_arena->attached_threads = 1;
    /* 原来的free_list 不怕是非空的？*/
  free_list = NULL; 
  for (mstate ar_ptr = &main_arena;; )
    {
      __libc_lock_init (ar_ptr->mutex);
      if (ar_ptr != thread_arena)
        {
          /* This arena is no longer attached to any thread.  */
          ar_ptr->attached_threads = 0;
          ar_ptr->next_free = free_list;
          free_list = ar_ptr;
        }
      ar_ptr = ar_ptr->next;
      if (ar_ptr == &main_arena)
        break;
    }

  __libc_lock_init (list_lock);
}

#if HAVE_TUNABLES
#if USE_TCACHE
#endif
#else
/* Initialization routine. */
/* 获取环境变量？*/
#include <string.h>
extern char **_environ;

static char * next_env_entry (char ***position)
{
  char **current = *position;
  char *result = NULL;

  while (*current != NULL)
    {
      if (__builtin_expect ((*current)[0] == 'M', 0)
          && (*current)[1] == 'A'
          && (*current)[2] == 'L'
          && (*current)[3] == 'L'
          && (*current)[4] == 'O'
          && (*current)[5] == 'C'
          && (*current)[6] == '_')
        {
          result = &(*current)[7];

          /* Save current position for next visit.  */
          *position = ++current;

          break;
        }

      ++current;
    }

  return result;
}
#endif

/*在 glibc 的源代码中，ptmalloc_init 函数负责初始化 ptmalloc 内存分配器的数据结构、锁、缓存等。
这个函数通常在程序启动时被调用，确保 glibc 内存分配器在程序运行期间能够正常工作。
具体来说，ptmalloc_init 函数会初始化一些全局变量，创建用于线程同步的锁（例如 mutex），并设置一些参数
，以便 ptmalloc 内存分配器能够根据多线程程序的需求进行合适的内存分配和释放操作。*/

static void  ptmalloc_init (void)
{
  if (__malloc_initialized >= 0)
    return;

  __malloc_initialized = 0;

  /* 为什么是main_arena ，不会子线程调用初始化吗 */
  thread_arena = &main_arena;

  malloc_init_state (&main_arena);

  /* 省略 删除 从环境变量读取 TOP_PAD MMAP_MAX ARENA_MAX ARENA_TEST TRIM_THRESHOLD TRIM_THRESHOLD */
 
  __malloc_initialized = 1;
}

/* Managing heaps and arenas (for concurrent threads) */

/* If consecutive mmap (0, HEAP_MAX_SIZE << 1, ...) calls return decreasing
   addresses as opposed to increasing, new_heap would badly fragment the
   address space.  In that case remember the second HEAP_MAX_SIZE part
   aligned to HEAP_MAX_SIZE from last mmap (0, HEAP_MAX_SIZE << 1, ...)
   call (if it is already aligned) and try to reuse it next time.  We need
   no locking for it, as kernel ensures the atomicity for us - worst case
   we'll call mmap (addr, HEAP_MAX_SIZE, ...) for some value of addr in
   multiple threads, but only one will succeed.  */
static char *aligned_heap_area;

/* New_heap()函数负责从 mmap 区域映射一块内存来作为 sub_heap，
在 64 为系统上，该函数映射 64M内存，映射的内存块地址按 64M 对齐。
New_heap()函数只是映射一块虚拟地址空间，该空间不可读写，不会被 swap。*/
static heap_info * new_heap (size_t size, size_t top_pad)
{
  size_t pagesize = GLRO (dl_pagesize);//也
  char *p1, *p2;
  unsigned long ul;
  heap_info *h;

  /* 调整 size 的大小，size 的最小值为 32K,
  最大值 HEAP_MAX_SIZE 在不同的系统上不同，64 位系统为 64M，
  将 size 的大小调整到最小值与最大值之间，并以页对齐，
  如果 size 大于最大值，直接报错。*/
  if (size + top_pad < HEAP_MIN_SIZE)
    size = HEAP_MIN_SIZE; //heap最小的大小是HEAP_MIN_SIZE
  else if (size + top_pad <= HEAP_MAX_SIZE)
    size += top_pad; 
  else if (size > HEAP_MAX_SIZE) //size不能比HEAP_MAX_SIZE大
    return 0;
  else
    size = HEAP_MAX_SIZE;  
  size = ALIGN_UP (size, pagesize);

  /* A memory region aligned to a multiple of HEAP_MAX_SIZE is needed.
     No swap space needs to be reserved for the following large
     mapping (on Linux, this is the case for all non-writable mappings
     anyway). */
  p2 = MAP_FAILED;
  /*全局变量 aligned_heap_area 是上一次调用 mmap 分配内存的结束虚拟地址，
  并已经按照 HEAP_MAX_SIZE 大小对齐。如果 aligned_heap_area 不为空，
  尝试从上次映射结束地址开始映射大小为 HEAP_MAX_SIZE 的内存块，*/
  if (aligned_heap_area)  
  {
    p2 = (char *) MMAP (aligned_heap_area, HEAP_MAX_SIZE, PROT_NONE,MAP_NORESERVE);
    /*无论映射是否成功，都将全局变量 aligned_heap_area 设置为 NULL。???*/
    aligned_heap_area = NULL; 
    /*由于全局变量 aligned_heap_area 没有锁保护，
    可能存在多个线程同时 mmap()函数从 aligned_heap_area 开始映射新的虚拟内存块，
    操作系统会保证只会有一个线程会成功，其它在同一地址映射新虚拟内存块都会失败。*/
    /*如果映射成功，但返回的虚拟地址不是按 HEAP_MAX_SIZE 大小对齐的，取消该区域的映射，映射失败。*/
    if (p2 != MAP_FAILED && ((unsigned long) p2 & (HEAP_MAX_SIZE - 1))) 
    {
      __munmap (p2, HEAP_MAX_SIZE);
      p2 = MAP_FAILED;
    }
  }
  if (p2 == MAP_FAILED)
    {
      /*如果第一次分配失败了，就会再尝试一次，这次分配HEAP_MAX_SIZE*2大小的内存，并且新内存的起始地址由内核决定。*/
      /*因为尝试分配了HEAP_MAX_SIZE*2大小的内存，其中必定包含了大小为HEAP_MAX_SIZE且和HEAP_MAX_SIZE对齐的内存*/
      p1 = (char *) MMAP (0, HEAP_MAX_SIZE << 1, PROT_NONE, MAP_NORESERVE); 
      if (p1 != MAP_FAILED)
        {
          p2 = (char *) (((unsigned long) p1 + (HEAP_MAX_SIZE - 1)) & ~(HEAP_MAX_SIZE - 1));
          ul = p2 - p1;
          if (ul)
            __munmap (p1, ul); //解除对齐后多余的映射
          else
            aligned_heap_area = p2 + HEAP_MAX_SIZE;
          __munmap (p2 + HEAP_MAX_SIZE, HEAP_MAX_SIZE - ul);//最后还需要将多余的虚拟内存还回给操作系统。
        }
      else //如果第二次分配失败 就会通过MMAP进行第三次分配，只分配HEAP_MAX_SIZE大小的内存，并且起始地址由内核决定，如果又失败了就返回0。
        {
          /* Try to take the chance that an allocation of only HEAP_MAX_SIZE
             is already aligned. */
          p2 = (char *) MMAP (0, HEAP_MAX_SIZE, PROT_NONE, MAP_NORESERVE);
          if (p2 == MAP_FAILED)
            return 0;

          if ((unsigned long) p2 & (HEAP_MAX_SIZE - 1))
          {
            __munmap (p2, HEAP_MAX_SIZE);
            return 0;
          }
        }
    }
  /*调用 mprotect()函数将 size 大小的内存设置为可读可写，如果失败，解除整个 sub_heap的映射。然后更新 heap_info 实例中的相关字段。*/
  if (__mprotect (p2, size, PROT_READ | PROT_WRITE) != 0)
    {
      __munmap (p2, HEAP_MAX_SIZE);
      return 0;
    }

  /*新 heap 开头 是 heap_info ？*/
  h = (heap_info *) p2;
  h->size = size;
  h->mprotect_size = size;
  return h;
}

/* 将 sub_heap 中可读可写区域扩大*/
static int grow_heap (heap_info *h, long diff)
{
  size_t pagesize = GLRO (dl_pagesize);
  long new_size;

  diff = ALIGN_UP (diff, pagesize);
  new_size = (long) h->size + diff;
  if ((unsigned long) new_size > (unsigned long) HEAP_MAX_SIZE)
    return -1;

  if ((unsigned long) new_size > h->mprotect_size)
    {
      if (__mprotect ((char *) h + h->mprotect_size,(unsigned long) new_size - h->mprotect_size,PROT_READ | PROT_WRITE) != 0)
        return -2;
      h->mprotect_size = new_size;
    }
  h->size = new_size;
  return 0;
}

/* 缩小 sub_heap 的虚拟内存区域 ，减小该 sub_heap 的虚拟内存占用量； */

static int shrink_heap (heap_info *h, long diff)
{
  long new_size;

  new_size = (long) h->size - diff;
  if (new_size < (long) sizeof (*h))
    return -1;

  /* 如果该函数运行在非 Glibc 中，则从 sub_heap 中切割出 diff 大小的虚拟内存，
  创建一个新的不可读写的映射区域，
  注意 mmap()函数这里使用了 MAP_FIXED 标志，然后更新 sub_heap 的可读可写内存大小。 */
  if (__glibc_unlikely (check_may_shrink_heap ()))
  {
    if ((char *) MMAP ((char *) h + new_size, diff, PROT_NONE, MAP_FIXED) == (char *) MAP_FAILED)
      return -2;
    h->mprotect_size = new_size;
  }
  else
  /*如果该函数运行在 Glibc 库中，则调用 madvise()函数，
  实际上 madvise()函数什么也不做，只是返回错误，这里并没有处理 madvise()函数的返回值。*/
    __madvise ((char *) h + new_size, diff, MADV_DONTNEED);
  /*fprintf(stderr, "shrink %p %08lx\n", h, new_size);*/

  h->size = new_size;
  LIBC_PROBE (memory_heap_less, 2, h, h->size);
  return 0;
}

/* sub_heap 的虚拟内存还回给操作系统； */

#define delete_heap(heap) \
  do {									      \
  /*判断当前删除的 sub_heap 的结束地址是否与全局变量aligned_heap_area 指向的地址相同，
  如果相同，则将全局变量 aligned_heap_area 设置为 NULL，
  因为当前 sub_heap 删除以后，就可以从当前 sub_heap 的起始地址或是更低的地址开始映射新的 sub_heap，
  这样可以尽量从地地址映射内存。*/
      if ((char *) (heap) + HEAP_MAX_SIZE == aligned_heap_area)  aligned_heap_area = NULL;					      \
      __munmap ((char *) (heap), HEAP_MAX_SIZE);			      \
    } while (0)

/*根据 sub_heap 的 top chunk 大小调用 shrink_heap()函数收缩 sub_heap */
static int  heap_trim (heap_info *heap, size_t pad)
{
  mstate ar_ptr = heap->ar_ptr;
  unsigned long pagesz = GLRO (dl_pagesize);
  mchunkptr top_chunk = top (ar_ptr), p;
  heap_info *prev_heap;
  long new_size, top_size, top_area, extra, prev_size, misalign;


  /*每个非主分配区至少有一个 sub_heap，
  每个非主分配区的第一个 sub_heap 中包含了一个 heap_info 的实例和 malloc_state 的实例，
  分主分配区中的其它 sub_heap 中只有一个heap_info 实例，
  紧跟 heap_info 实例后，为可以用于分配的内存块。
  当当前非主分配区的 top chunk 与当前 sub_heap 的 heap_info 实例的结束地址相同时，
  意味着当前 sub_heap 中只有一个空闲 chunk，没有已分配的 chunk。
  所以可以将当前整个 sub_heap 都释放掉。*/
  /* Can this heap go away completely? */
  while (top_chunk == chunk_at_offset (heap, sizeof (*heap)))
    {
      /*每个 sub_heap 的可读可写区域的末尾都有两个 chunk 用于 fencepost，
      以 64 位系统为例，最后一个 chunk 占用的空间为 MINSIZE-2*SIZE_SZ，为 16B，
      最后一个 chuk 的 size 字段记录的前一个 chunk 为 inuse 状态，
      并标识当前 chunk 大小为 0，倒数第二个 chunk 为 inuse状态，
      这个 chunk 也是 fencepost 的一部分，这个 chunk 的大小为 2*SIZE_SZ，为 16B，
      所以用于 fencepost 的两个 chunk 的空间大小为 32B*/
      prev_heap = heap->prev;
      prev_size = prev_heap->size - (MINSIZE - 2 * SIZE_SZ);
      /*获取最后一个chunk的地址*/
      p = chunk_at_offset (prev_heap, prev_size);
      /* fencepost must be properly aligned.  */
      misalign = ((long) p) & MALLOC_ALIGN_MASK;
      p = chunk_at_offset (prev_heap, prev_size - misalign);
      assert (chunksize_nomask (p) == (0 | PREV_INUSE)); /* must be fencepost */
      /*获取倒数第二个chunk的地址*/
      p = prev_chunk (p);
      new_size = chunksize (p) + (MINSIZE - 2 * SIZE_SZ) + misalign;
      assert (new_size > 0 && new_size < (long) (2 * MINSIZE));
      /*如果倒数第二个 chunk 的前一个chunk 为空闲状态，当前 sub_heap 中可读可写区域还需要加上这个空闲chunk 的大小*/
      if (!prev_inuse (p))
        new_size += prev_size (p);
      assert (new_size > 0 && new_size < HEAP_MAX_SIZE);
      /*如果 new_size 与 sub_heap 中剩余的不可读写的区域大小之和小于 32+4K（64位系统），
      意味着前一个 sub_heap 的可用空间太少了，不能释放当前的 sub_heap。*/
      if (new_size + (HEAP_MAX_SIZE - prev_heap->size) < pad + MINSIZE + pagesz)
        break;
      /*首先更新非主分配区的内存统计，然后调用 delete_heap()宏函数释放该 sub_heap，*/
      ar_ptr->system_mem -= heap->size;
      delete_heap (heap);
      /*把当前 heap 设置为被释放 sub_heap 的前一个 sub_heap，*/
      heap = prev_heap;
      /*如果 p 的前一个 chunk 为空闲状态，*/
      if (!prev_inuse (p)) /* consolidate backward */
        {
          /*由于不可能出现多个连续的空闲 chunk， p 指向空闲 chunk，
          并将该空闲 chunk 从空闲 chunk 链表中移除*/
          p = prev_chunk (p);
          unlink_chunk (ar_ptr, p);
        }
      assert (((unsigned long) ((char *) p + new_size) & (pagesz - 1)) == 0);
      assert (((char *) p + new_size) == ((char *) heap + heap->size));
      /*该空闲 chunk 赋值给 sub_heap 的 top chunk*/
      top (ar_ptr) = top_chunk = p;
      set_head (top_chunk, new_size | PREV_INUSE);
      /*check_chunk(ar_ptr, top_chunk);*/
    }

  /* 首先查看 top chunk 的大小，如果 top chunk 的大小减去 pad 和 MINSIZE 小于一页大小，返回退出，
  否则调用 shrink_heap()函数对当前 sub_heap 进行收缩，将空闲的整数个页收缩掉，仅剩下不足一页的空闲内存，
  如果 shrink_heap()失败，返回退出，否则，更新内存使用统计，更新 top chunk 的大小。 */
  top_size = chunksize (top_chunk);
  if ((unsigned long)(top_size) < (unsigned long)(mp_.trim_threshold))
    return 0;

  top_area = top_size - MINSIZE - 1;
  if (top_area < 0 || (size_t) top_area <= pad)
    return 0;

  /* Release in pagesize units and round down to the nearest page.  */
  extra = ALIGN_DOWN(top_area - pad, pagesz);
  if (extra == 0)
    return 0;

  /* Try to shrink. */
  if (shrink_heap (heap, extra) != 0)
    return 0;

  ar_ptr->system_mem -= extra;

  /* Success. Adjust top accordingly. */
  set_head (top_chunk, (top_size - extra) | PREV_INUSE);
  /*check_chunk(ar_ptr, top_chunk);*/
  return 1;
}

/* Create a new arena with initial size "size".  */

/* If REPLACED_ARENA is not NULL, detach it from this thread.  Must be
   called while free_list_lock is held.  */
static void detach_arena (mstate replaced_arena)
{
  if (replaced_arena != NULL)
    {
      assert (replaced_arena->attached_threads > 0);
      /* The current implementation only detaches from main_arena in
	 case of allocation failure.  This means that it is likely not
	 beneficial to put the arena on free_list even if the
	 reference count reaches zero.  */
      --replaced_arena->attached_threads;
    }
}
/*创建一个非主分配区，在 arena_get2()函数中被调用*/
static mstate _int_new_arena (size_t size)
{
  /*对于一个新的非主分配区，至少包含一个 sub_heap，
  每个非主分配区中都有相应的管理数据结构，每个非主分配区都有一个 heap_info 实例和 malloc_state 的实例，
  这两个实例都位于非主分配区的第一个sub_heap的开始部分，malloc_state实例紧接着heap_info实例。
  所以在创建非主分配区时，需要为管理数据结构分配额外的内存空间。
  New_heap()函数创建一个新的 sub_heap，并返回 sub_heap 的指针*/
  mstate a;
  heap_info *h;
  char *ptr;
  unsigned long misalign;

  h = new_heap (size + (sizeof (*h) + sizeof (*a) + MALLOC_ALIGNMENT), mp_.top_pad);
  if (!h)
    {
      /* Maybe size is too large to fit in a single heap.  So, just try
         to create a minimally-sized arena and let _int_malloc() attempt
         to deal with the large request via mmap_chunk().  */
      h = new_heap (sizeof (*h) + sizeof (*a) + MALLOC_ALIGNMENT, mp_.top_pad);
      if (!h)
        return 0;
    }
  a = h->ar_ptr = (mstate) (h + 1);
  malloc_init_state (a);
  a->attached_threads = 1;
  /*a->next = NULL;*/
  a->system_mem = a->max_system_mem = h->size;

  /* 在 sub_heap 中 malloc_state 实例后的内存可以分配给用户使用，
  ptr 指向存储malloc_state 实例后的空闲内存，对 ptr 按照 2*SZ_SIZE 对齐后，
  将 ptr 赋值给分配区的 top chunk，也就是说把 sub_heap 中整个空闲内存块作为 top chunk，
  然后设置 top chunk 的 size，并标识 top chunk 的前一个 chunk 为已处于分配状态。 */
  ptr = (char *) (a + 1);
  misalign = (unsigned long) chunk2mem (ptr) & MALLOC_ALIGN_MASK;
  if (misalign > 0)
    ptr += MALLOC_ALIGNMENT - misalign;
  top (a) = (mchunkptr) ptr;
  set_head (top (a), (((char *) h + h->size) - ptr) | PREV_INUSE);

  LIBC_PROBE (memory_arena_new, 2, a, size);
  mstate replaced_arena = thread_arena;
  /*将创建好的非主分配区加入线程的私有实例中，然后对非主分配区的锁进行初始化，并获得该锁。*/
  thread_arena = a;
  __libc_lock_init (a->mutex);
  __libc_lock_lock (list_lock);
  /* 将刚创建的非主分配区加入到分配区的全局链表中 ，这里修改全局分配区链表时需要获得全局锁list_lock */
  a->next = main_arena.next;
  /* FIXME: The barrier is an attempt to synchronize with read access
     in reused_arena, which does not acquire list_lock while
     traversing the list.  */
  atomic_write_barrier ();
  main_arena.next = a;
  
  __libc_lock_unlock (list_lock);

  __libc_lock_lock (free_list_lock);
  detach_arena (replaced_arena);/*????????*/
  __libc_lock_unlock (free_list_lock);

  /* Lock this arena.  NB: Another thread may have been attached to
     this arena because the arena is now accessible from the
     main_arena.next list and could have been picked by reused_arena.
     This can only happen for the last arena created (before the arena
     limit is reached).  At this point, some arena has to be attached
     to two threads.  We could acquire the arena lock before list_lock
     to make it less likely that reused_arena picks this new arena,
     but this could result in a deadlock with
     __malloc_fork_lock_parent.  */

  __libc_lock_lock (a->mutex);

  return a;
}

/* 从free_list摘取一个arena */
static mstate get_free_list (void)
{
  mstate replaced_arena = thread_arena;
  mstate result = free_list;
  if (result != NULL)
    {
      __libc_lock_lock (free_list_lock);
      result = free_list;
      /*首先查看 arena 的 free_list 中是否为 NULL，如果不为 NULL，获得全局锁 list_lock，将 free_list 的第一个 arena 从单向链表中取出，解锁 list_lock。*/
      if (result != NULL)
      {
        free_list = result->next_free;//摘除当前节点
        /* The arena will be attached to this thread.  */
        assert (result->attached_threads == 0);
        result->attached_threads = 1;
        detach_arena (replaced_arena);
      }
      __libc_lock_unlock (free_list_lock);
      /*如果从free_list 中获得一个 arena，对该 arena 加锁，并将该 arena 加入线程的私有实例中*/
      if (result != NULL)
        {
          LIBC_PROBE (memory_arena_reuse_free_list, 1, result);
          __libc_lock_lock (result->mutex);
	        thread_arena = result;
        }
    }

  return result;
}

/* 从 free_list 中移除一个指定的 arena */
static void remove_from_free_list (mstate arena)
{
  mstate *previous = &free_list;
  for (mstate p = free_list; p != NULL; p = p->next_free)
    {
      assert (p->attached_threads == 0);
      if (p == arena)
      {
        /* Remove the requested arena from the list.  */
        *previous = p->next_free;
        break;
      }
      else
	      previous = &p->next_free;
    }
}

/* 如果分配区全部处于忙碌中，则通过遍历方式，尝试没有加锁的分配区进行分配操作。
如果得到一个没有加锁的分配区，则attached_threads关联的线程数，并将thread_arena设置到当前的分配区上。
这样就实现了多线程环境下，分配区的重复利用。  */
static mstate reused_arena (mstate avoid_arena)
{
  mstate result;
  /* FIXME: Access to next_to_use suffers from data races.  */
  static mstate next_to_use; /*静态变量*/
  if (next_to_use == NULL)
    next_to_use = &main_arena;

  /* Iterate over all arenas (including those linked from
     free_list).  */
  result = next_to_use;
  /*寻找一个不能锁定的分配区*/
  do
    {
      if (!__libc_lock_trylock (result->mutex))
        goto out;

      /* FIXME: This is a data race, see _int_new_arena.  */
      result = result->next;
    }
  while (result != next_to_use);

  /* Avoid AVOID_ARENA as we have already failed to allocate memory
     in that arena and it is currently locked.   */
  if (result == avoid_arena)
    result = result->next;

  /* 尝试加锁都失败了，等待获得 next_to_use 指向的分配区的锁  */
  LIBC_PROBE (memory_arena_reuse_wait, 3, &result->mutex, result, avoid_arena);
  __libc_lock_lock (result->mutex);

out:
  /*执行到 out 的代码，意味着已经获得一个分配区的锁*/
  /* Attach the arena to the current thread.  */
  {
    /* Update the arena thread attachment counters.   */
    mstate replaced_arena = thread_arena;
    __libc_lock_lock (free_list_lock);
    detach_arena (replaced_arena);

    /* We may have picked up an arena on the free list.  We need to
       preserve the invariant that no arena on the free list has a
       positive attached_threads counter (otherwise,
       arena_thread_freeres cannot use the counter to determine if the
       arena needs to be put on the free list).  We unconditionally
       remove the selected arena from the free list.  The caller of
       reused_arena checked the free list and observed it to be empty,
       so the list is very short.  */
    remove_from_free_list (result);

    ++result->attached_threads;

    __libc_lock_unlock (free_list_lock);
  }

  LIBC_PROBE (memory_arena_reuse, 2, result, avoid_arena);
  /*将该分配区加入线程私有实例*/
  thread_arena = result;
  /*将当前分配区的下一个分配区赋值给 next_to_use*/
  next_to_use = result->next;

  return result;
}

/* 获取一个free arena或者创建一个size的arena */
static mstate arena_get2 (size_t size, mstate avoid_arena) 
{
  mstate a;

  static size_t narenas_limit;

  a = get_free_list ();   
  if (a == NULL)  //如果arena free_list是空
  {
    /* Nothing immediately available, so generate a new arena.  */
    if (narenas_limit == 0)
      {
        if (mp_.arena_max != 0)
          narenas_limit = mp_.arena_max;
        else if (narenas > mp_.arena_test)
          {
            int n = __get_nprocs ();

            if (n >= 1)
              narenas_limit = NARENAS_FROM_NCORES (n);
            else
              /* We have no information about the system.  Assume two
                  cores.  */
              narenas_limit = NARENAS_FROM_NCORES (2);
          }
      }
  repeat:;
    size_t n = narenas;
    /* NB: the following depends on the fact that (size_t)0 - 1 is a
        very large number and that the underflow is OK.  If arena_max
        is set the value of arena_test is irrelevant.  If arena_test
        is set but narenas is not yet larger or equal to arena_test
        narenas_limit is 0.  There is no possibility for narenas to
        be too big for the test to always fail since there is not
        enough address space to create that many arenas.  */
    if (__glibc_unlikely (n <= narenas_limit - 1))
    {
      if (catomic_compare_and_exchange_bool_acq (&narenas, n + 1, n))
        goto repeat;
      a = _int_new_arena (size);
      if (__glibc_unlikely (a == NULL))
        catomic_decrement (&narenas);
    }
    else
      a = reused_arena (avoid_arena);
  }
  return a;
}

/* 如果我们没有main_arena，那么失败可能是由于mmapped区域用完，所以我们可以尝试在main_arena上分配。
  否则，sbrk（）很可能已经失败，仍然有机会进行mmap（），所以请尝试其他arena。  */
static mstate arena_get_retry (mstate ar_ptr, size_t bytes)
{
  LIBC_PROBE (memory_arena_retry, 2, bytes, ar_ptr);
  if (ar_ptr != &main_arena)
    {
      __libc_lock_unlock (ar_ptr->mutex);
      ar_ptr = &main_arena;
      __libc_lock_lock (ar_ptr->mutex);
    }
  else
    {
      __libc_lock_unlock (ar_ptr->mutex);
      ar_ptr = arena_get2 (bytes, ar_ptr);
    }
  return ar_ptr;
}
/*  解除attache 线程私有实例arena，如果attached_threads为0，则将arena放到free list*/
void __malloc_arena_thread_freeres (void)
{
  /* Shut down the thread cache first.  This could deallocate data for
     the thread arena, so do this before we put the arena on the free
     list.  */
  tcache_thread_shutdown ();

  mstate a = thread_arena;
  thread_arena = NULL;

  if (a != NULL)
  {
    __libc_lock_lock (free_list_lock);
          /* If this was the last attached thread for this arena, put the
      arena on the free list.  */
    assert (a->attached_threads > 0);
    if (--a->attached_threads == 0)
    {
      a->next_free = free_list;
      free_list = a;
    }
    __libc_lock_unlock (free_list_lock);
  }
}

/*
 * Local variables:
 * c-basic-offset: 2
 * End:
 */
