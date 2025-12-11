#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include <lauxlib.h>

#include "skynet.h"
#include "atomic.h"

#include "malloc_hook.h"

// turn on MEMORY_CHECK can do more memory check, such as double free
// #define MEMORY_CHECK

#define MEMORY_ALLOCTAG 0x20140605
#define MEMORY_FREETAG 0x0badf00d


// alignas(CACHE_LINE_SIZE) 这是 C11 标准的对齐说明符，强制 handle 成员（以及整个结构体）
// 按照 CACHE_LINE_SIZE（缓存行大小，通常为 64 字节）对齐
// 避免 “缓存行伪共享”
struct mem_data {
    alignas(CACHE_LINE_SIZE)
	ATOM_ULONG     handle; // 存储与当前内存统计槽位关联的 “服务句柄” 绑定内存分配数据到具体服务
    AtomicMemInfo  info;
};
_Static_assert(sizeof(struct mem_data) % CACHE_LINE_SIZE == 0, "mem_data must be cache-line aligned");

// 在内存分配时附加额外的元数据
//  内存跟踪机制的核心元数据结构 通过在用户申请的内存块前附加该结构体
struct mem_cookie {
	size_t size; // 分配的内存大小
	uint32_t handle; // 服务句柄
#ifdef MEMORY_CHECK
	uint32_t dogtag; // 仅在定义 MEMORY_CHECK 宏时存在，用于内存完整性校验：
#endif
 // 明确记录当前 mem_cookie 结构体自身的大小
	uint32_t cookie_size;	// should be the last
};

#define SLOT_SIZE 0x10000
#define PREFIX_SIZE sizeof(struct mem_cookie)

// 按服务跟踪内存使用的核心数据结构
// 关联服务与内存统计
// 高效统计
// 空间与精度平衡

static struct mem_data mem_stats[SLOT_SIZE];
_Static_assert(alignof(mem_stats) % CACHE_LINE_SIZE == 0, "mem_stats must be cache-line aligned");

// 通过服务句柄（handle）计算哈希索引，定位到 mem_stats 数组中对应的 mem_data 结构体，
// 从而获取该服务的内存分配统计信息（如分配 / 释放的字节数、块数等）
static struct mem_data *
get_mem_stat(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));
	struct mem_data *data = &mem_stats[h];
	return data;
}

#ifndef NOUSE_JEMALLOC // 如果没有定义不使用jemalloc, 即默认用jemalloc

#include "jemalloc.h"

// for skynet_lalloc use
#define raw_realloc je_realloc
#define raw_free je_free

inline static void
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	struct mem_data *data = get_mem_stat(handle);
    // 当两个不同的 handle 被哈希到同一个槽位时, 新的服务会覆盖旧服务的数据
    // 这种情况在实际运行中非常罕见, 因为同时存在的服务数量很难超过 65536
    ATOM_STORE(&data->handle, handle);
	atomic_meminfo_alloc(&data->info, __n);
}

inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	struct mem_data *data = get_mem_stat(handle);
	atomic_meminfo_free(&data->info, __n);
}

// 当通过 skynet_malloc、skynet_realloc 等函数分配内存时，
// 会在用户实际使用的内存块前附加一段元数据（mem_cookie），
// fill_prefix 负责初始化这段元数据并返回用户可用的内存地址
inline static void*
fill_prefix(char* ptr, size_t sz, uint32_t cookie_size) {
	// 获取当前执行内存分配操作的skynet服务句柄，用于将内存块与服务绑定
	uint32_t handle = skynet_current_handle();

	// 定位元数据结构体
	struct mem_cookie *p = (struct mem_cookie *)ptr; // 将原始内存块起始地址 ptr 强制转换为 mem_cookie 指针，作为元数据的起始位置
	char * ret = ptr + cookie_size; // 计算用户可用内存的起始地址 ret：原始地址加上元数据大小 cookie_size
	sz += cookie_size;  // 计算总内存大小（用户数据 + 元数据）
	p->size = sz; // 记录总内存大小（释放时需用）
	p->handle = handle; // 绑定当前服务句柄
#ifdef MEMORY_CHECK
	p->dogtag = MEMORY_ALLOCTAG; // 内存校验标记（分配时设为特定值）
#endif
	// 更新内存统计，当前服务的内存配分统计，字节数
	update_xmalloc_stat_alloc(handle, sz);
	// 记录元数据大小
	memcpy(ret - sizeof(uint32_t), &cookie_size, sizeof(cookie_size));
	return ret;
}

inline static uint32_t
get_cookie_size(char *ptr) {
	uint32_t cookie_size;
	memcpy(&cookie_size, ptr - sizeof(cookie_size), sizeof(cookie_size));
	return cookie_size;
}

inline static void*
clean_prefix(char* ptr) {
	uint32_t cookie_size = get_cookie_size(ptr);
	struct mem_cookie *p = (struct mem_cookie *)(ptr - cookie_size);
	uint32_t handle = p->handle;
#ifdef MEMORY_CHECK
	uint32_t dogtag = p->dogtag;
	if (dogtag == MEMORY_FREETAG) {
		fprintf(stderr, "xmalloc: double free in :%08x\n", handle);
	}
	assert(dogtag == MEMORY_ALLOCTAG);	// memory out of bounds
	p->dogtag = MEMORY_FREETAG;
#endif
	update_xmalloc_stat_free(handle, p->size);
	return p;
}

static void malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n",
		size);
	fflush(stderr);
	abort(); // 内存不够，强制退出程序
}

void
memory_info_dump(const char* opts) {
	je_malloc_stats_print(0,0, opts);
}

bool
mallctl_bool(const char* name, bool* newval) {
	bool v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(bool));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	return v;
}

int
mallctl_cmd(const char* name) {
	return je_mallctl(name, NULL, NULL, NULL, 0);
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	size_t v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(size_t));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	// skynet_error(NULL, "name: %s, value: %zd\n", name, v);
	return v;
}

int
mallctl_opt(const char* name, int* newval) {
	int v = 0;
	size_t len = sizeof(v);
	if(newval) {
		int ret = je_mallctl(name, &v, &len, newval, sizeof(int));
		if(ret == 0) {
			skynet_error(NULL, "set new value(%d) for (%s) succeed\n", *newval, name);
		} else {
			skynet_error(NULL, "set new value(%d) for (%s) failed: error -> %d\n", *newval, name, ret);
		}
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}

	return v;
}

// hook : malloc, realloc, free, calloc

void *
skynet_malloc(size_t size) {
	void* ptr = je_malloc(size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr, size, PREFIX_SIZE);
}

void *
skynet_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return skynet_malloc(size);

	uint32_t cookie_size = get_cookie_size(ptr);
	void* rawptr = clean_prefix(ptr);
	void *newptr = je_realloc(rawptr, size+cookie_size);
	if(!newptr) malloc_oom(size);
	return fill_prefix(newptr, size, cookie_size);
}

void
skynet_free(void *ptr) {
	if (ptr == NULL) return;
	void* rawptr = clean_prefix(ptr);
	je_free(rawptr);
}

void *
skynet_calloc(size_t nmemb, size_t size) {
	uint32_t cookie_n = (PREFIX_SIZE+size-1)/size;
	void* ptr = je_calloc(nmemb + cookie_n, size);
	if(!ptr) malloc_oom(nmemb * size);
	return fill_prefix(ptr, nmemb * size, cookie_n * size);
}

static inline uint32_t
alignment_cookie_size(size_t alignment) {
	if (alignment >= PREFIX_SIZE)
		return alignment;
	switch (alignment) {
	case 4 :
		return (PREFIX_SIZE + 3) / 4 * 4;
	case 8 :
		return (PREFIX_SIZE + 7) / 8 * 8;
	case 16 :
		return (PREFIX_SIZE + 15) / 16 * 16;
	}
	return (PREFIX_SIZE + alignment - 1) / alignment * alignment;
}

void *
skynet_memalign(size_t alignment, size_t size) {
	uint32_t cookie_size = alignment_cookie_size(alignment);
	void* ptr = je_memalign(alignment, size + cookie_size);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr, size, cookie_size);
}

void *
skynet_aligned_alloc(size_t alignment, size_t size) {
	uint32_t cookie_size = alignment_cookie_size(alignment);
	void* ptr = je_aligned_alloc(alignment, size + cookie_size);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr, size, cookie_size);
}

int
skynet_posix_memalign(void **memptr, size_t alignment, size_t size) {
	uint32_t cookie_size = alignment_cookie_size(alignment);
	int err = je_posix_memalign(memptr, alignment, size + cookie_size);
	if (err) malloc_oom(size);
	fill_prefix(*memptr, size, cookie_size);
	return err;
}

#else

// for skynet_lalloc use
#define raw_realloc realloc
#define raw_free free

void
memory_info_dump(const char* opts) {
	skynet_error(NULL, "No jemalloc");
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_int64 %s.", name);
	return 0;
}

int
mallctl_opt(const char* name, int* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_opt %s.", name);
	return 0;
}

bool
mallctl_bool(const char* name, bool* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_bool %s.", name);
	return 0;
}

int
mallctl_cmd(const char* name) {
	skynet_error(NULL, "No jemalloc : mallctl_cmd %s.", name);
	return 0;
}

#endif

size_t
malloc_used_memory(void) {
	MemInfo total = {};
	for(int i = 0; i < SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		const uint32_t handle = ATOM_LOAD(&data->handle);
		if (handle != 0) {
			atomic_meminfo_merge(&total, &data->info);
		}
	}
	return total.alloc - total.free;
}

size_t
malloc_memory_block(void) {
	MemInfo total = {};
	for(int i = 0; i < SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		const uint32_t handle = ATOM_LOAD(&data->handle);
		if (handle != 0) {
			atomic_meminfo_merge(&total, &data->info);
		}
	}
	return total.alloc_count - total.free_count;
}

// 遍历所有内存统计槽位（mem_stats 数组），收集并打印每个有效服务的内存使用数据，
// 最后汇总并输出总内存使用量
// c 层的内存使用统计
void
dump_c_mem() {
	// 初始化与日志头
	skynet_error(NULL, "dump all service mem:");
	MemInfo total = {};
	for(int i = 0; i < SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		const uint32_t handle = ATOM_LOAD(&data->handle);
		if (handle != 0) {
			// 处理有效服务的内存信息
			MemInfo info = {};
			// 收集单个服务的内存信息
			atomic_meminfo_merge(&info, &data->info);
			meminfo_merge(&total, &info);
			const size_t using = info.alloc - info.free;
			skynet_error(NULL, ":%08x -> %zukb %zub", handle, using >> 10, using);
		}
	}
	// 输出总内存使用量
	const size_t using = total.alloc - total.free;
	skynet_error(NULL, "+total: %zukb", using >> 10);
}

char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	char * ret = skynet_malloc(sz+1);
	memcpy(ret, str, sz+1);
	return ret;
}

void *
skynet_lalloc(void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		raw_free(ptr);
		return NULL;
	} else {
		return raw_realloc(ptr, nsize);
	}
}

// 内存统计返回到lua层，供lua脚本层使用
int
dump_mem_lua(lua_State *L) {
	int i;
	lua_newtable(L);
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		const uint32_t handle = ATOM_LOAD(&data->handle);
		if (handle != 0) {
			MemInfo info = {};
			atomic_meminfo_merge(&info, &data->info);
			lua_pushinteger(L, info.alloc - info.free);
			lua_rawseti(L, -2, handle);
		}
	}
	return 1;
}

size_t
malloc_current_memory(void) {
	uint32_t handle = skynet_current_handle();
	struct mem_data *data = get_mem_stat(handle);
	if (ATOM_LOAD(&data->handle) != handle) {
		return 0;
	}
	MemInfo info = {};
	atomic_meminfo_merge(&info, &data->info);
	return info.alloc - info.free;
}

void
skynet_debug_memory(const char *info) {
	// for debug use
	uint32_t handle = skynet_current_handle();
	size_t mem = malloc_current_memory();
	fprintf(stderr, "[:%08x] %s %p\n", handle, info, (void *)mem);
}
