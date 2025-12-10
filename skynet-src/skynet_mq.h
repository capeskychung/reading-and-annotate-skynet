#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	uint32_t source;
	int session;
	void * data;
	size_t sz;
};

// type is encoding in skynet_message.sz high 8bit
#define MESSAGE_TYPE_MASK (SIZE_MAX >> 8)
#define MESSAGE_TYPE_SHIFT ((sizeof(size_t)-1) * 8)

struct message_queue;

// 全局消息队列push函数
void skynet_globalmq_push(struct message_queue * queue);
struct message_queue * skynet_globalmq_pop(void); // 全局消息队列pop函数

struct message_queue * skynet_mq_create(uint32_t handle); // 消息队列创建接口
void skynet_mq_mark_release(struct message_queue *q);

typedef void (*message_drop)(struct skynet_message *, void *);

void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud); // mq释放
uint32_t skynet_mq_handle(struct message_queue *); // 获取消息队列对应的handle

// 0 for success
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message); // 从消息队列中获取消息
void skynet_mq_push(struct message_queue *q, struct skynet_message *message); // 将消息放进消息队列

// return the length of message queue, for debug
int skynet_mq_length(struct message_queue *q); // 获取一个消息队列的长度
int skynet_mq_overload(struct message_queue *q); // 消息队列是否 overloaded

void skynet_mq_init();

#endif
