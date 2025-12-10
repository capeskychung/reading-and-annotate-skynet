#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

// 每个service维护一个私有的消息队列，时实现服务间通信的核心数据结构
struct message_queue {
	struct spinlock lock; // 自旋锁（spinlock），用于保证多线程操作消息队列时的线程安全
	uint32_t handle; //	关联的服务句柄（handle），即当前消息队列所属服务的唯一标识
	int cap; // 消息队列的容量（即可以容纳的最大消息数量）
	int head; // 消息队列的头指针，指向队列中第一个消息的位置
	int tail; // 消息队列的尾指针，指向队列中最后一个消息的下一个位置
	int release; // 释放标记（0：正常；1：待释放）
	int in_global; // 全局队列标识（0：不在全局队列；1：在全局队列或正在调度）
	int overload; // 消息队列是否处于 overloaded 状态
	int overload_threshold; // 消息队列的 overloaded 阈值 默认1024
	struct skynet_message *queue; // 消息队列数组，用于存储实际的消息数据

	// 用于将多个 message_queue 串联成链表（主要用于全局消息队列 global_queue 的存储，global_queue 是一个链表结构）。
	struct message_queue *next;
};

// 全局消息队列，单链表结构
// head 用于获取下一个待处理的服务消息队列，tail 用于高效地将新队列追加到全局队列末尾
struct global_queue {
	struct message_queue *head;
	struct message_queue *tail;
	struct spinlock lock;
};

static struct global_queue *Q = NULL;

// 新增一个service的时候，将消息队列放进全局消息队列
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL); // 只有全局消息队列的next才有用，普通的消息队列next应为null
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else { // 全局消息队列是空的
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

// 从全局消息队列中去除一个消息队列
struct message_queue * 
skynet_globalmq_pop() {
	// 局部变量的访问在编译器层面可能被优化（如寄存器缓存），相比直接访问全局变量，可能带来微小的性能提升
	struct global_queue *q = Q; 

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		q->head = mq->next;
		if(q->head == NULL) { // 全局消息队列为空
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL; // 将消息队列的 next 指针置为 NULL，表示该消息队列已从全局消息队列中移除
	}
	SPIN_UNLOCK(q)

	return mq;
}

// 创建一个消息队列， 是否进入全局队列交由外部处理，每个函数只做自己的事情，简单化
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	// 服务初始化好之后，会在skynet_context_new  中 调用skynet_globalmq_push 将其推入全局队列。
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

// 获取消息队列的长度
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}

int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	SPIN_LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) {
			q->head = head = 0;
		}
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2; // 消息队列的 overloaded 阈值翻倍
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	
	skynet_free(q->queue);
	q->queue = new_queue;
}

void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) {
		expand_queue(q);
	}

	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}

void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
