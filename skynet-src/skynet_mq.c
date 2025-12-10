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

// 从指定消息队列中取出消息的核心函数
// 从队列头部提取消息，更新队列状态（如头部指针、过载阈值），并在队列空时标记队列退出全局队列
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1; // 默认为1（表示队列空，未取出消息）
	SPIN_LOCK(q) // 加自旋锁，保证多线程操作队列的安全性

	// 若队列非空（头指针 != 尾指针）
	if (q->head != q->tail) {
		// 取出队头消息并移动头指针
		*message = q->queue[q->head++];
		ret = 0; // 成功取出消息，返回值设为0

		// 缓存当前头、尾指针和队列容量（避免重复访问结构体成员）
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		// 若头指针超出容量，循环回到队列起始位置（环形队列特性）
		if (head >= cap) {
			q->head = head = 0;
		}
		// 计算当前队列中的消息数量
		int length = tail - head;
		if (length < 0) { // 当头指针 > 尾指针时（环形队列绕回），需加上容量计算真实长度
			length += cap;
		}
		// 处理过载阈值：若当前长度超过阈值，则更新过载标记并翻倍阈值
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2; // 消息队列的 overloaded 阈值翻倍
		}
	} else {
		// reset overload_threshold when queue is empty
		// 队列空时，重置过载阈值为默认值（MQ_OVERLOAD = 1024）
		q->overload_threshold = MQ_OVERLOAD;
	}

	// 若未取出消息（队列空），标记队列不在全局队列中
	if (ret) {
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

// 当消息队列（环形缓冲区）已满时，将队列容量翻倍，确保新消息能继续入队，避免消息丢失
static void
expand_queue(struct message_queue *q) {
	// 分配新的队列缓冲区，容量为当前的2倍
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	// 将旧队列中的消息按顺序复制到新队列
	for (i=0;i<q->cap;i++) {
		// 计算旧队列中第i个有效消息的位置（环形队列特性）
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}

	// 重置新队列的头指针为0（消息从新队列起始位置开始）
	q->head = 0;
	// 新队列的尾指针设为旧队列的容量（因为复制了旧队列全部消息）
	q->tail = q->cap;
	// 更新队列容量为原来的2倍
	q->cap *= 2;
	
	skynet_free(q->queue);
	q->queue = new_queue;
}


// 向指定消息队列（message_queue）插入消息的核心函数
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message); // 确保消息指针非空，避免无效操作
	SPIN_LOCK(q) // 加自旋锁，保证多线程插入消息的安全性

	// 将消息存入队列尾部（tail 指针位置）
	q->queue[q->tail] = *message;
	// 尾指针后移，若超出队列容量则重置为0（环形队列特性）
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	// 若头指针与尾指针重合，说明队列已满，触发扩容
	if (q->head == q->tail) {
		expand_queue(q);
	}

	// 若队列当前不在全局队列中（in_global=0），则将其加入全局队列
	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL; // 标记为已加入全局队列
		skynet_globalmq_push(q); // 插入全局队列
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

// 标记消息队列（message_queue）为 “待释放” 状态，
// 并确保该队列被加入全局队列，以便后续由 skynet_mq_release 函数处理其释放逻辑
void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0); // 确保队列未被标记过释放，避免重复操作
	q->release = 1; // 将队列标记为待释放状态

	// 若队列当前不在全局队列中，则将其加入全局队列
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

// 用于彻底清除消息队列及其包含的消息资源
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	// // 循环从队列中弹出消息，直到队列为空（skynet_mq_pop 返回非0表示弹出失败）
	while(!skynet_mq_pop(q, &msg)) {
		// // 调用外部传入的回调函数处理弹出的消息（如释放消息内容）
		drop_func(&msg, ud);
	}
	// 释放队列自身的内存资源
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) { // 如果标记为待释放
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud); // 执行队列清理和释放
	} else {  // 若队列未标记为待释放
		skynet_globalmq_push(q); // 将队列重新加入全局队列，等待后续调度
		SPIN_UNLOCK(q)
	}
}
