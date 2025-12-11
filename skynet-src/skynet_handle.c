#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_imp.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000

struct handle_name {
	char * name; // 服务的名称（字符串指针）
	uint32_t handle;  // 服务的唯一标识符（32位无符号整数）
};

struct handle_storage {
	struct rwlock lock; // 读写锁，用于多线程安全访问结构体数据

	uint32_t harbor; // 集群节点标识（港口号），用于生成全局唯一handle
	uint32_t handle_index; // 下一个待分配的handle基础值（本地编号部分）
	int slot_size; // 哈希表（slot数组）的当前容量
	struct skynet_context ** slot; // 哈希表数组，存储服务上下文指针，通过handle哈希定位

	int name_cap; // 服务名称数组的容量（预分配的最大数量）
	int name_count; // 当前已注册的服务名称数量
	struct handle_name *name;  // 服务名称与handle的映射数组（按名称排序，支持二分查找）
};

// 服务句柄管理的核心全局变量
static struct handle_storage *H = NULL;

// 为 Skynet 框架中的服务（skynet_context）注册并分配一个全局唯一的句柄（handle）
uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock); // 写锁

	for (;;) {
		int i;
		// 尝试分配句柄
		uint32_t handle = s->handle_index;
		for (i=0;i<s->slot_size;i++,handle++) {
			if (handle > HANDLE_MASK) {
				// 0 is reserved
				handle = 1; // 0 为系统保留，从 1 重新开始
			}
			int hash = handle & (s->slot_size-1); // 计算哈希值（利用位运算高效取模）
			if (s->slot[hash] == NULL) { // 找到空槽位
				s->slot[hash] = ctx; // 存储服务指针
				s->handle_index = handle + 1; // 更新下一次分配的起始值

				rwlock_wunlock(&s->lock); // 释放写锁

				handle |= s->harbor;  // 拼接集群节点标识，生成全局唯一句柄
				return handle; // 返回分配的句柄
			}
		}
		// 未找到空槽位，需要扩容
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);  // 确保扩容后不超过最大限制
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));

		// 迁移旧哈希表数据到新表
		for (i=0;i<s->slot_size;i++) {
			if (s->slot[i]) {
				int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1); // 新哈希值
				assert(new_slot[hash] == NULL); // 确保新槽位为空，避免冲突
				new_slot[hash] = s->slot[i]; // 迁移服务指针
			}
		}
		skynet_free(s->slot); // 释放旧哈希表内存
		s->slot = new_slot; // 更新指向新哈希表
		s->slot_size *= 2; // 容量翻倍
	}
}

// 销毁服务的核心函数，负责从句柄管理系统中移除指定服务的句柄（handle）映射，并释放相关资源，确保服务退出后系统状态的一致性
int
skynet_handle_retire(uint32_t handle) {
	// 初始化与加锁
	int ret = 0;
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);

	// 定位服务上下文
	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	// 验证并移除服务
	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		// 从哈希表中移除服务
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				// // 释放服务名称的内存
				skynet_free(s->name[i].name);
				continue; // 跳过当前条目（不保留）
			} else if (i!=j) {
				// 前移非目标条目，覆盖被删除的位置
				s->name[j] = s->name[i];
			}
			++j;
		}
		// 更新有效名称数量
		s->name_count = j;
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.

		// 释放服务上下文（解锁后操作，避免嵌套锁冲突）
		skynet_context_release(ctx);
	}

	return ret;
}

void
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx) {
				handle = skynet_context_handle(ctx);
				++n;
			}
			rwlock_runlock(&s->lock);
			if (handle != 0) {
				skynet_handle_retire(handle);
			}
		}
		if (n==0)
			return;
	}
}

struct skynet_context *
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

uint32_t
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		int i;
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	char * result = skynet_strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

const char *
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}


// 函数负责初始化该结构体，设置初始容量和哈希表
void
skynet_handle_init(int harbor) {
	assert(H==NULL);
	struct handle_storage * s = skynet_malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	rwlock_init(&s->lock);
	// reserve 0 for system
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}
