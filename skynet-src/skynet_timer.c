#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

/*
定时器模块的核心管理结构，采用分层时间轮设计：
near 数组：存储即将到期的定时器（未来 0~255 厘秒内），时间粒度为 1 厘秒。
t 二维数组：4 级远程定时器，每级覆盖更大的时间范围（通过 TIME_LEVEL_SHIFT 控制层级粒度），用于管理远期到期的定时器，减少每次检查的复杂度。
时间相关字段：time 是内部计数基准，current 和 current_point 用于关联系统时间与内部计数，starttime 记录框架启动的绝对时间。
自旋锁 lock：保证多线程对定时器数据操作的原子性（如添加 / 删除定时器节点
*/

// 用于存储定时器到期时需要触发的事件信息，包含要通知的服务句柄和对应的会话 ID，当定时器到期时，会基于这些信息向目标服务发送消息。
struct timer_event {
	uint32_t handle; // 目标服务的句柄（Skynet 中标识服务的唯一 ID）
	int session; // 会话 ID（用于标识定时器事件的回调关联）
};


// 定时器节点的基础结构，每个定时器任务对应一个节点，next 指针用于将节点接入链表
struct timer_node {
	struct timer_node *next;  // 链表节点指针，用于将多个定时器节点串联
	uint32_t expire; // 定时器到期时间（基于内部时间计数器的厘秒值）
};

// 管理多个 timer_node。通过头节点（固定哨兵）和尾指针，实现高效的节点插入（直接在尾部添加）和清空操作
struct link_list {
	struct timer_node head;  // 链表头节点（哨兵节点，简化链表操作）
	struct timer_node *tail;  // 链表尾指针（快速定位链表末尾，优化插入效率）
};

// 数组大小为 256，每个元素是一个链表（link_list），按任务的到期时间哈希到不同链表中（通过 expire & TIME_NEAR_MASK 计算索引，TIME_NEAR_MASK 为 0xFF）。
struct timer {
	struct link_list near[TIME_NEAR]; // 存储近期到期的定时任务（即将在 TIME_NEAR 个时间单位内触发） 256
	struct link_list t[4][TIME_LEVEL]; // 用于存储远期到期的定时任务，分为 4 个层级（第一维），每个层级包含 TIME_LEVEL 64个链表（第二维）。
	//多级结构的设计是为了优化定时器管理效率：
	//第 0 级：覆盖 256 ~ 256+64*256 厘秒
	//第 1 级：覆盖更大的时间范围，以此类推
    // 同样通过 link_clear 初始化每个链表，确保初始状态为空。	
    struct spinlock lock; // 自旋锁，多线程环境下保护定时器数据
	uint32_t time;  // 内部时间计数器（厘秒级，从 0 开始递增）
	uint32_t starttime; // 框架启动时的秒级时间（基于 CLOCK_REALTIME）
	uint64_t current;  // 框架运行的总厘秒数（相对时间）
	uint64_t current_point; // 基于单调时钟的当前厘秒数（用于计算时间差）
};

static struct timer * TI = NULL;

// 函数返回被清空的节点链表（以第一个节点为起始）
static inline struct timer_node *
link_clear(struct link_list *list) {
	// 保存链表中当前的第一个有效节点（头节点的next指向的节点）
	struct timer_node * ret = list->head.next;

	 // 将头节点的next指针置空，断开与后续节点的连接
	list->head.next = 0;
	// 将尾指针重新指向头节点（此时链表为空，尾节点即头节点）
	list->tail = &(list->head);
	// 返回被移除的节点链表
	return ret;
}

// 将一个定时器节点（timer_node）添加到链表（link_list）的尾部
static inline void
link(struct link_list *list,struct timer_node *node) {
	// 将新节点挂到链表尾节点的 next 指针上
	list->tail->next = node; 
	// 更新链表的尾指针，使其指向新节点（新节点成为新的尾节点）
	list->tail = node;
	// 新节点的 next 指针置空，确保它是链表的最后一个节点
	node->next=0;
}

// 将定时器节点（timer_node）插入到 Skynet 定时器模块的分层时间轮结构
static void
add_node(struct timer *T,struct timer_node *node) {
	// 获取节点的到期时间（内部厘秒计数器值）和当前时间计数器值
	uint32_t time=node->expire;
	uint32_t current_time=T->time;

	// 判断是否属于近程定时器（即将在 0~255 厘秒内到期）
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		// 插入近程链表数组（near），索引为到期时间的低 8 位（0~255）
		link(&T->near[time&TIME_NEAR_MASK],node);
	} else {
		int i;
		// 初始化远程定时器的掩码（初始覆盖 256~256+64*256 厘秒范围）
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		// 遍历 4 级远程定时器中的前 3 级，寻找合适的层级
		for (i=0;i<3;i++) {
			// 判断当前层级是否能覆盖该到期时间
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break; // 找到匹配的层级，退出循环
			}
			// 掩码左移，扩大覆盖范围，进入下一级
			mask <<= TIME_LEVEL_SHIFT;
		}
		// 计算该层级中具体的链表索引，插入对应的远程链表
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}

// 添加一个新的定时任务
static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {
	// 分配内存：定时器节点本身大小 + 附加参数大小
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);
	// 将附加参数（定时任务信息）复制到节点后面的内存区域
	memcpy(node+1,arg,sz);
	// 加自旋锁，保证多线程操作的安全性
	SPIN_LOCK(T);
	// 计算定时器到期时间：当前内部时间 + 延迟时间（time 单位为厘秒）
		node->expire=time+T->time;
	 // 将节点插入到时间轮的对应层级链表中
		add_node(T,node);

	SPIN_UNLOCK(T);
}

// 将指定层级和索引的远程定时器链表中的所有节点重新分配到时间轮的合适位置
static void
move_list(struct timer *T, int level, int idx) {
	 // 清空目标层级（level）和索引（idx）的链表，并获取该链表的所有节点
	struct timer_node *current = link_clear(&T->t[level][idx]);

	// 遍历所有被清空的节点
	while (current) {
		// 保存当前节点的下一个节点（避免后续操作修改指针后丢失）
		struct timer_node *temp=current->next;
		// 将当前节点重新插入时间轮（由 add_node 决定新的位置）
		add_node(T,current);
		// 移动到下一个节点
		current=temp;
	}
}


/*
时间轮（Time Wheel）是一种高效的定时器管理数据结构，
通过多层级的 “时间槽” 存储不同到期时间的节点。
timer_shift 函数的核心功能是：当内部时间计数器递增时，
判断是否需要将高层级时间槽中的节点迁移到低层级，
确保节点随着时间推进逐步靠近 “到期区”。
*/

// TIME_NEAR_SHIFT = 8，TIME_NEAR = 1 << 8 = 256：近程时间轮的大小（256 个时间槽，单位为厘秒）
// TIME_LEVEL_SHIFT = 6，TIME_LEVEL = 1 << 6 = 64：每个远程层级的时间槽数量（64 个）
// TIME_NEAR_MASK = 255（TIME_NEAR - 1）：用于计算近程时间槽索引的掩码
// TIME_LEVEL_MASK = 63（TIME_LEVEL - 1）：用于计算远程层级时间槽索引的掩码
static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR; // 初始掩码为近程时间轮大小（256）
	uint32_t ct = ++T->time; // 内部时间计数器 +1，并记录当前值（ct 为新时间）
	if (ct == 0) {
		 // 若时间计数器溢出（从最大值回到 0），触发最高层级（3 级）的 0 号槽迁移
		move_list(T, 3, 0);
	} else {
		// 计算当前时间在远程层级中的基准值（右移 8 位，即除以 256）
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i=0; // 远程层级索引（0~3）

		// 循环检查是否需要迁移各层级的节点
		while ((ct & (mask-1))==0) {
			// 计算当前层级（i）的时间槽索引
			int idx=time & TIME_LEVEL_MASK;
			if (idx!=0) {
				 // 若索引不为 0，迁移该层级、该索引的所有节点，然后退出循环
				move_list(T, i, idx);
				break;				
			}

			// 若索引为 0，继续检查更高层级
			mask <<= TIME_LEVEL_SHIFT; // 掩码左移 6 位（扩大 64 倍）
			time >>= TIME_LEVEL_SHIFT; // 时间基准值右移 6 位（缩小 64 倍）
			++i; // 层级 +1（向更高层级检查）
		}
	}
}

// 处理到期定时器事件的核心逻辑，负责将到期的定时器节点转换为消息并分发，同时释放节点内存
static inline void
dispatch_list(struct timer_node *current) {
	do {
		 // 1. 从定时器节点中提取事件数据
		// current 是定时器节点，其内存布局为：[timer_node][timer_event]
		// current+1 指向节点后附加的 timer_event 结构体
		struct timer_event * event = (struct timer_event *)(current+1);

		// 2. 构造 Skynet 消息
		struct skynet_message message;
		message.source = 0;  // 消息源为 0（表示是定时器模块发送的系统消息）
		message.session = event->session; // 携带定时器的 session 标识（用于回调匹配）
		message.data = NULL; // 该定时器消息无需附加数据

		// 消息类型：PTYPE_RESPONSE（响应类型），左移 MESSAGE_TYPE_SHIFT 位存储类型信息
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		// 3. 将消息推送到目标服务的消息队列
        // event->handle 是目标服务的句柄，skynet_context_push 负责将消息入队
		skynet_context_push(event->handle, &message);
		
		// 4. 释放当前定时器节点的内存
		struct timer_node * temp = current;  // 保存当前节点指针
		current=current->next; // 移动到下一个节点
		skynet_free(temp);	 // 释放当前节点（包含 timer_node 和 timer_event）
	} while (current);  // 循环处理链表中的所有节点
}

// 执行到期定时器事件的核心逻辑
static inline void
timer_execute(struct timer *T) {
	// 1. 计算当前到期的近程时间槽索引
    // TIME_NEAR_MASK 是 255（TIME_NEAR-1），通过与运算获取 T->time 的低 8 位
    // 对应近程时间轮（256 个槽）中当前时间所在的槽位
	int idx = T->time & TIME_NEAR_MASK;

	// 2. 循环处理该槽位中所有节点（可能有多个定时器同时到期）
    // 检查链表是否非空（head.next 不为 NULL 表示有节点）
	while (T->near[idx].head.next) {
		// 2.1 清空该槽位的链表并取出所有节点（原子操作，避免后续插入干扰）
		struct timer_node *current = link_clear(&T->near[idx]);

		// 2.2 释放定时器锁，避免分发过程阻塞其他定时器操作
		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T

		// 2.3 分发节点：将定时器事件转换为消息并发送到目标服务，释放节点内存
        // 此处无需持有锁，因为节点已被移出时间轮，且操作与时间轮核心逻辑解耦
		dispatch_list(current);

		 // 2.4 重新加锁，继续处理可能新加入该槽位的节点（循环检查）
		SPIN_LOCK(T);
	}
}

// 协调时间推进和事件处理的核心调度函数，负责在时间更新时触发定时器节点的迁移和到期事件的执行
static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);  // 加锁，保证定时器操作的线程安全

	// try to dispatch timeout 0 (rare condition)
	// 1. 处理可能的 0 超时节点（罕见情况）
	timer_execute(T);

	// shift time first, and then dispatch timer message
	// 2. 推进时间轮，迁移高层级节点到低层级
	timer_shift(T);

	// 3. 处理时间推进后新到期的节点
	timer_execute(T);

	SPIN_UNLOCK(T);  // 解锁，允许其他操作访问定时器
}

static struct timer *
timer_create_timer() {
	// 分配内存并初始化
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r)); // 内存块清零，确保所有字段初始化为默认值

	int i,j;

	// 初始化近程定时器链表（near 数组） 表示近程定时器的时间粒度（厘秒级，1/100 秒
	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	// 初始化多级远程定时器链表（t 数组）
	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	SPIN_INIT(r) // 初始化自旋锁

	r->current = 0; // 初始化定时器的当前时间计数器

	return r;
}

// 提供定时任务功能的核心接口，用于向指定服务（handle）注册一个定时事件，当超时时间到达后，目标服务会收到一个通知消息
int
skynet_timeout(uint32_t handle, int time, int session) {
	// 处理立即超时的情况（time <= 0）
	if (time <= 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;
	 	// 将消息推送到目标服务的消息队列
        // 若推送失败（如服务已销毁），返回 -1
		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		// 处理延迟超时的情况（time > 0）
        // 构造定时器事件，存储目标服务句柄和 session
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		// 将事件添加到定时器中，time 毫秒后触发
		timer_add(TI, &event, sizeof(event), time);
	}
	// 返回 session 标识（无论立即还是延迟，均返回原 session）
	return session;
}

// systime 函数通过系统调用获取当前实时时间（CLOCK_REALTIME），并转换为秒（sec）和厘秒（cs，1/100 秒）
// 将获取到的起始秒数存入 TI->starttime（框架启动时的秒级时间），厘秒数存入 TI->current（框架运行的相对厘秒数，作为定时器的基准时间
// centisecond: 1/100 second
static void
systime(uint32_t *sec, uint32_t *cs) {
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec; // 获取系统时间 秒
	*cs = (uint32_t)(ti.tv_nsec / 10000000); // 当前系统时间对应的厘秒 1/100 秒
}

// 获取当前时间，厘秒
static uint64_t
gettime() {
	uint64_t t;
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
	return t;
}

// 定时器模块的 “时间推进器”，负责根据系统实际时间差更新定时器状态，确保定时任务按预期触发
void
skynet_updatetime(void) {
	// 1. 获取当前系统单调时间（单位：厘秒，1/100 秒）
	uint64_t cp = gettime();

	// 2. 处理时间回退的异常情况（极少发生，可能由系统时间调整导致）
	if(cp < TI->current_point) {
		// 记录错误日志：时间从当前记录值回退到了更小的值
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		// 强制更新记录的时间点，避免后续计算异常
		TI->current_point = cp;
	// 3. 正常情况：当前时间晚于上次记录时间，计算时间差并推进定时器
	} else if (cp != TI->current_point) {
		// 计算与上次记录的时间差（单位：厘秒）
		uint32_t diff = (uint32_t)(cp - TI->current_point);
		// 更新记录的时间点为当前时间
		TI->current_point = cp;
		// 更新定时器的当前时间计数（累计总厘秒数）
		TI->current += diff;
		int i;
		// 4. 按时间差逐个推进定时器，确保每个单位时间都被处理
		for (i=0;i<diff;i++) {
			timer_update(TI); // 每次调用处理一个厘秒的定时器事件
		}
	}
}

uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

uint64_t 
skynet_now(void) {
	return TI->current;
}

// 定时器的初始化函数
void 
skynet_timer_init(void) {
	TI = timer_create_timer(); // 创建定时器实例
	uint32_t current = 0;
	systime(&TI->starttime, &current); // 设置定时器启动时间和当前时间，启动时间是秒，当前时间是毫秒
	TI->current = current; // 框架运行的相对厘秒数，作为定时器的基准时间
	TI->current_point = gettime(); // 用于后续定时器更新时计算时间差
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

uint64_t
skynet_thread_time(void) {
	// 1. 定义 timespec 结构体，用于存储时间信息（秒和纳秒）
	struct timespec ti;
	// 2. 获取当前线程的 CPU 时间（CLOCK_THREAD_CPUTIME_ID 表示线程的 CPU 时钟）
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	// 3. 将时间转换为微秒并返回
	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
}
