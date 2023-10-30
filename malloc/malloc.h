/* malloc实现的原型和定义。
] */

#ifndef _MALLOC_H
#define _MALLOC_H 1

#include <features.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _LIBC
# define __MALLOC_HOOK_VOLATILE //__MALLOC_HOOK_VOLATILE 是一个用于 GNU C Library（glibc）的内部宏。这个宏主要用于插入自定义的内存分配器钩子（malloc hooks）到 glibc 的内存分配器中。
# define __MALLOC_DEPRECATED  //__MALLOC_DEPRECATED 是一个用于标记特定函数或变量已被废弃（deprecated）的宏。当一个函数或变量被标记为废弃时，编译器会在使用该函数或变量时发出警告，提示开发者该函数或变量已经不推荐使用，可能会在将来的版本中被移除。
#else
# define __MALLOC_HOOK_VOLATILE volatile //内存分配器钩子是用于跟踪、记录或修改内存分配和释放的函数，允许程序员在内存操作发生时执行自定义的代码。__MALLOC_HOOK_VOLATILE 宏用于声明这些钩子函数的类型。
# define __MALLOC_DEPRECATED __attribute_deprecated__ 
#endif


__BEGIN_DECLS  //__BEGIN_DECLS 是一个宏，通常用于 C 语言中，用于在 C++ 环境中使用 C 函数。

/* Allocate SIZE bytes of memory.  */
extern void *malloc (size_t __size) __THROW __attribute_malloc__    //__THROW 是一个在 GNU C 库（glibc）中使用的宏，用于标记函数不会抛出异常。在 C 语言中，函数默认是不会抛出异常的。
     __attribute_alloc_size__ ((1)) __wur;                  //__attribute_malloc__ 是GNU C编译器（例如GCC）提供的一个特殊属性（attribute）。它用于告知编译器，标记的函数返回的指针应该被当作动态分配的内存，即函数的返回值是malloc、calloc等动态分配内存的函数的返回指针。
                                                                              //当你使用 __attribute_malloc__ 时，它会帮助编译器进行静态分析，以便在编译时检查函数的返回值是否被检查，是否被释放等，以便发现潜在的内存泄漏问题。
                                                                              ///???????函数实现在哪里？？？

/* Allocate NMEMB elements of SIZE bytes each, all initialized to 0.  */
extern void *calloc (size_t __nmemb, size_t __size)  
__THROW __attribute_malloc__ __attribute_alloc_size__ ((1, 2)) __wur; 
 
/* Re-allocate the previously allocated block in __ptr, making the new
   block SIZE bytes long.  */
/* __attribute_malloc__ is not used, because if realloc returns
   the same pointer that was passed to it, aliasing needs to be allowed
   between objects pointed by the old and new pointers.  */
extern void *realloc (void *__ptr, size_t __size)  //realloc 函数的作用是将之前分配的内存块的大小改变为新指定的大小。如果 ptr 是 NULL，则 realloc 的行为类似于 malloc(size)，分配一个新的内存块。如果 size 是 0，realloc 的行为类似于 free(ptr)，释放内存块，并返回 NULL。
__THROW __attribute_warn_unused_result__ __attribute_alloc_size__ ((2)); //如果 ptr 不是 NULL，realloc 尝试将之前分配的内存块的大小修改为 size。它可能会在原地扩展内存块，也可能会将内存块移动到一个新的位置。在内存块被移动时，原内存块中的数据会被复制到新的内存块中。


/* Re-allocate the previously allocated block in PTR, making the new
   block large enough for NMEMB elements of SIZE bytes each.  */
/* __attribute_malloc__ is not used, because if reallocarray returns
   the same pointer that was passed to it, aliasing needs to be allowed
   between objects pointed by the old and new pointers.  */
extern void *reallocarray (void *__ptr, size_t __nmemb, size_t __size)  
__THROW __attribute_warn_unused_result__ __attribute_alloc_size__ ((2, 3));

/* Free a block allocated by `malloc', `realloc' or `calloc'.  */
extern void free (void *__ptr) __THROW;

/* Allocate SIZE bytes allocated to ALIGNMENT bytes.  */
extern void *memalign (size_t __alignment, size_t __size) //memalign 函数是用于在内存中分配一块指定大小的内存空间，其起始地址是指定对齐数（alignment）的倍数。memalign 函数不属于C标准库的一部分，而是一些Unix-like操作系统（例如Linux）提供的系统调用或GNU C库提供的扩展函数。
__THROW __attribute_malloc__ __attribute_alloc_size__ ((2)) __wur;

/* Allocate SIZE bytes on a page boundary.  */
extern void *valloc (size_t __size) __THROW __attribute_malloc__   //valloc 函数是一个用于在内存中分配一块指定大小的内存空间的函数。它在某些UNIX-like操作系统中可用，但是并不是C标准库的一部分。
     __attribute_alloc_size__ ((1)) __wur;

/* Equivalent to valloc(minimum-page-that-holds(n)), that is, round up
   __size to nearest pagesize. */
extern void *pvalloc (size_t __size) __THROW __attribute_malloc__  //pvalloc 函数是一个在一些UNIX-like系统中提供的用于分配内存的函数，用于分配指定大小的内存块，并将其起始地址对齐到页面（page）大小的整数倍上。页面大小通常是操作系统的虚拟内存管理单元的大小，通常为4KB或更大。
     __attribute_alloc_size__ ((1)) __wur;

/* Underlying allocation function; successive calls should return
   contiguous pieces of memory.  */   
extern void *(*__morecore) (ptrdiff_t __size);    //extern void *(*__morecore) (ptrdiff_t __size); 是一个函数指针声明，通常出现在C语言的内存分配实现中。这行代码声明了一个名为 __morecore 的函数指针，该指针指向一个接受 ptrdiff_t 类型参数并返回 void* 类型指针的函数。在内存分配实现中，__morecore 函数通常用于请求额外的内存空间，例如通过系统调用向操作系统请求更多内存。
                                                                        //在实际使用中，__morecore 函数指针通常会指向一个用于请求内存的底层系统调用，例如 sbrk（在一些Unix-like系统中用于扩展进程的数据段）或 mmap（用于在内存中映射文件或匿名内存块）。或者，它可能指向一个自定义的分配函数，具体取决于实现和需求。
/* Default value of `__morecore'.  */
extern void *__default_morecore (ptrdiff_t __size)
__THROW __attribute_malloc__;

/* SVID2/XPG mallinfo structure */

struct mallinfo  //struct mallinfo 是定义在 <malloc.h> 头文件中的结构体，它包含了关于内存分配器的统计信息。在C语言中，mallinfo 结构体用于获取动态内存分配器（如 malloc、calloc、realloc 等函数）的运行时状态，包括已分配的内存块数、已分配的总字节数、空闲的内存块数、空闲的总字节数等。
{
  int arena;    /* non-mmapped space allocated from system */
  int ordblks;  /* number of free chunks */
  int smblks;   /* number of fastbin blocks */
  int hblks;    /* number of mmapped regions */
  int hblkhd;   /* space in mmapped regions */
  int usmblks;  /* always 0, preserved for backwards compatibility */
  int fsmblks;  /* space available in freed fastbin blocks */
  int uordblks; /* total allocated space */
  int fordblks; /* total free space */
  int keepcost; /* top-most, releasable (via malloc_trim) space */
};

/* Returns a copy of the updated current mallinfo. */ 
extern struct mallinfo mallinfo (void) __THROW;  //使用 mallinfo 函数来获取当前内存分配器的统计信息

/* SVID2/XPG mallopt options */
#ifndef M_MXFAST
# define M_MXFAST  1    /* maximum request size for "fastbins" */ 
#endif
#ifndef M_NLBLKS
# define M_NLBLKS  2    /* UNUSED in this malloc */
#endif
#ifndef M_GRAIN
# define M_GRAIN   3    /* UNUSED in this malloc */
#endif
#ifndef M_KEEP
# define M_KEEP    4    /* UNUSED in this malloc */
#endif

/* mallopt options that actually do something */
#define M_TRIM_THRESHOLD    -1  //M_TRIM_THRESHOLD 是定义在 <malloc.h> 头文件中的一个宏，用于指定在 malloc_trim 函数中的阈值。malloc_trim 函数用于尝试将底层内存管理器（例如 glibc 中的 malloc 实现）的内存占用降至最低，通过释放空闲的内存块给操作系统。malloc_trim 函数的调用通常会在应用程序释放大量内存后，以便将内存返回给操作系统。M_TRIM_THRESHOLD 宏指定了释放内存的阈值。如果剩余的空闲内存大小（即空闲的内存块总字节数）大于该阈值，malloc_trim 函数就会尝试将内存返回给操作系统。
#define M_TOP_PAD           -2     //M_TOP_PAD 是定义在 <malloc.h> 头文件中的一个宏，它用于指定 malloc 函数在分配大块内存时（通常是通过系统调用 mmap 分配的），在所请求的内存大小之上额外分配的内存空间，以便提高性能和减少碎片化。在 glibc 中，默认的 M_TOP_PAD 值通常是 0。这意味着，malloc 函数在请求大块内存时，只会分配所需大小的内存，没有额外的内存用于填充。
#define M_MMAP_THRESHOLD    -3
#define M_MMAP_MAX          -4
#define M_CHECK_ACTION      -5
#define M_PERTURB           -6
#define M_ARENA_TEST        -7
#define M_ARENA_MAX         -8

/* General SVID/XPG interface to tunable parameters. */
extern int mallopt (int __param, int __val) __THROW;

/* Release all but __pad bytes of freed top-most memory back to the
   system. Return 1 if successful, else 0. */
extern int malloc_trim (size_t __pad) __THROW;

/* Report the number of usable allocated bytes associated with allocated
   chunk __ptr. */
extern size_t malloc_usable_size (void *__ptr) __THROW;

/* Prints brief summary statistics on stderr. */
extern void malloc_stats (void) __THROW;

/* Output information about state of allocator to stream FP.  */
extern int malloc_info (int __options, FILE *__fp) __THROW;

/* Hooks for debugging and user-defined versions. */
extern void (*__MALLOC_HOOK_VOLATILE __free_hook) (void *__ptr,
                                                   const void *)
__MALLOC_DEPRECATED;
extern void *(*__MALLOC_HOOK_VOLATILE __malloc_hook)(size_t __size,
                                                     const void *)
__MALLOC_DEPRECATED;
extern void *(*__MALLOC_HOOK_VOLATILE __realloc_hook)(void *__ptr,
                                                      size_t __size,
                                                      const void *)
__MALLOC_DEPRECATED;
extern void *(*__MALLOC_HOOK_VOLATILE __memalign_hook)(size_t __alignment,
                                                       size_t __size,
                                                       const void *)
__MALLOC_DEPRECATED;
extern void (*__MALLOC_HOOK_VOLATILE __after_morecore_hook) (void);


__END_DECLS
#endif /* malloc.h */
