#ifndef skynet_malloc_h
#define skynet_malloc_h

#include <stddef.h>

#define skynet_malloc malloc
#define skynet_calloc calloc
#define skynet_realloc realloc
#define skynet_free free
#define skynet_memalign memalign
#define skynet_aligned_alloc aligned_alloc
#define skynet_posix_memalign posix_memalign

void * skynet_malloc(size_t sz);
void * skynet_calloc(size_t nmemb,size_t size);
void * skynet_realloc(void *ptr, size_t size);
void skynet_free(void *ptr);
void * skynet_lalloc(void *ptr, size_t osize, size_t nsize);	// use for lua
void * skynet_memalign(size_t alignment, size_t size);
void * skynet_aligned_alloc(size_t alignment, size_t size);
int skynet_posix_memalign(void **memptr, size_t alignment, size_t size);

#endif

/*
在 skynet_malloc.h 中，既使用宏定义将 skynet_xxx 映射到标准库函数（如 malloc），又声明对应的函数原型，是为了兼顾接口统一性和实现灵活性，具体原因如下：
1. 宏定义的作用：快速映射与默认实现
宏定义（如 #define skynet_malloc malloc）的核心作用是提供默认实现：
当 Skynet 框架未启用自定义内存分配器（如 jemalloc）时，通过宏直接将框架的内存操作接口（skynet_malloc 等）映射到标准 C 库的内存函数（malloc 等），避免重复编写基础实现。
这种方式可以快速适配系统默认的内存管理机制，简化框架的基础依赖。
2. 函数声明的作用：确保接口一致性与扩展能力
函数声明（如 void * skynet_malloc(size_t sz);）的核心作用是定义统一接口，为后续扩展预留空间：
接口规范：无论底层使用标准库还是自定义内存分配器（如 jemalloc），框架中所有模块都通过 skynet_malloc 等函数进行内存操作，保证了接口的一致性。
覆盖宏定义：当需要启用自定义内存分配器时（如在 malloc_hook.c 中实现了基于 jemalloc 的 skynet_malloc），函数的实际实现会覆盖宏定义的默认映射。此时宏定义会被忽略，框架自动切换到自定义实现。
编译检查：函数声明为编译器提供了类型检查依据，确保调用者传递的参数类型与内存函数的要求一致，减少类型错误。
3. 两者结合的必要性
宏定义提供了 “零成本” 的默认实现，简化了框架在基础环境下的编译和使用。
函数声明则定义了抽象接口，允许框架在不修改上层调用代码的情况下，无缝切换到底层内存分配器（如从标准库切换到 jemalloc 以提升性能）。
例如，在 malloc_hook.c 中，当启用 jemalloc 时，会重新实现 skynet_malloc 等函数（实际调用 je_malloc），此时宏定义会被函数实现覆盖，框架自动使用 jemalloc 的功能；而如果关闭 jemalloc，宏定义会生效，退回到标准库实现。
*/