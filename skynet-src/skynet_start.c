#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// 管理工作线程的监控和同步，是线程调度与状态管理的核心数据结构
struct monitor {
	int count;  // 记录工作线程（worker thread）的总数 skynet 会根据配置的线程数（config.thread）初始化该值，用于遍历或管理所有工作线程
	struct skynet_monitor ** m; // 指向一个 skynet_monitor 结构体指针数组，数组长度为 count（与工作线程数一致） 是用于监控单个工作线程状态的结构（如检测服务是否陷入死循环），因此 m[i] 对应第 i 个工作线程的监控器
	pthread_cond_t cond; // 条件变量（POSIX 线程库），用于唤醒等待的工作线程。当有任务需要处理时（如消息队列中有消息），通过该变量通知休眠的线程
	pthread_mutex_t mutex; // 互斥锁（POSIX 线程库），与 cond 配合使用，确保对共享状态（如 sleep、quit）的操作线程安全，避免并发冲突
	int sleep; // 记录当前处于休眠状态的工作线程数量。当工作线程无任务可处理时，会进入休眠并递增该值；被唤醒时则递减
	int quit; // 退出标志位，用于通知所有工作线程终止运行。当框架需要退出时，该值被设为 1，工作线程检测到后会退出循环
};
/*
struct monitor 是工作线程的 “管理器”，主要功能包括：
维护工作线程与监控器的对应关系（通过 count 和 m）。
利用条件变量和互斥锁实现工作线程的休眠 / 唤醒机制（通过 cond、mutex、sleep），减少无意义的 CPU 空转。
统一控制工作线程的退出（通过 quit 标志）
*/


// 用于传递工作线程（worker thread）初始化参数的数据结构，主要作用是将线程启动所需的关键信息打包传递给工作线程的入口函数 thread_worker。
struct worker_parm {
	struct monitor *m; // 关联到全局的监控器结构体，工作线程通过它访问线程同步机制（如条件变量、互斥锁）和监控器数组，实现线程间的协调与状态监控
	int id; // 工作线程的编号，从0开始，用于定位该县城对应的监控器
	int weight; // 线程的权重值，影响消息调度策略，在 skynet_context_message_dispatch 中，权重决定了线程处理消息队列的贪婪程度（权重越高可能处理更多消息再休眠），用于负载均衡
};

static volatile int SIG = 0;

static void
handle_hup(int signal) { // 信号处理函数，参数为接收到的信号值
	if (signal == SIGHUP) {  // 仅处理 SIGHUP 信号
		SIG = 1;  // 将全局变量 SIG 设为 1，标记信号到来
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

// 封装了 POSIX 线程创建逻辑的工具函数 create_thread，用于简化在 Skynet 框架中简化线程创建过程并处理错误
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	// thread 用于存储新创建线程的标识符
	// start_routine 函数指针，指向线程的入口函数（线程启动后会执行该函数
	// arg 传递给线程入口函数的参数
	if (pthread_create(thread,NULL, start_routine, arg)) { // 调用 POSIX 线程库的 pthread_create 函数创建线程，传入上述参数。pthread_create 的第二个参数为线程属性（此处设为 NULL，使用默认属性）
		// 返回非 0 值
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

// 唤醒休眠工作线程
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) { // 当 休眠的线程数（m->sleep） 大于等于 总线程数减去需要保持活跃的线程数（m->count - busy） 时，触发唤醒操作
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond); // 执行唤醒操作 发送信号，唤醒一个正在 pthread_cond_wait 中休眠的工作线程（从 thread_worker 函数的逻辑可知，休眠线程会等待该条件变量
	}
}

// 网络事件处理线程的入口函数，负责监听和处理网络事件（如 socket 连接、数据收发等），并协调工作线程处理相关消息 // 有网络事件时，唤醒工作线程处理
static void *
thread_socket(void *p) {
	struct monitor * m = p; // 接收监控器结构体指针，用于线程间同步
	skynet_initthread(THREAD_SOCKET); // 初始化线程属性（标记为网络线程）
	for (;;) {
		int r = skynet_socket_poll();  // 轮询网络事件
		if (r==0)
			break; // 返回 0 表示需要退出，跳出循环
		if (r<0) { // 返回负值表示暂时无事件或出错
			CHECK_ABORT // 检查是否所有服务都已退出，若则退出循环
			continue; // 继续下一次轮询
		}
		wakeup(m,0); // 有网络事件时，唤醒工作线程处理
	}
	return NULL;
}

// 释放 struct monitor 结构体及其相关资源，是 Skynet 框架退出阶段的清理函数，确保线程同步资源和监控器数据被正确释放，避免内存泄漏
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count; // 获取工作线程总数（监控器数量）
	// 释放每个工作线程对应的监控器实例
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]); // 销毁单个监控器（内部可能释放监控器关联的资源）
	}

	// 销毁互斥锁和条件变量（线程同步机制）
	pthread_mutex_destroy(&m->mutex); // 释放互斥锁资源，避免系统资源泄漏
	pthread_cond_destroy(&m->cond); // 释放条件变量资源
	skynet_free(m->m); // 释放存储监控器指针数组的内存（m->m 是动态分配的数组）
	skynet_free(m); // 释放 monitor 结构体本身的内存
}

// 监控线程入口函数，负责定期检查所有工作线程的运行状态，确保工作线程未陷入异常（如死锁、无限循环等）
static void *
thread_monitor(void *p) {
	struct monitor * m = p; // 接收监控器结构体指针，包含所有工作线程的监控实例
	int i; 
	int n = m->count; // 工作线程总数
	skynet_initthread(THREAD_MONITOR); // 初始化线程属性（标记为监控线程）
	for (;;) { // 无限循环，持续监控
		CHECK_ABORT // 检查是否所有服务都已退出，若则退出循环
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]); // 检查单个工作线程的状态
		}

		// 每轮检查后休眠5秒，降低监控线程的资源消耗
		for (i=0;i<5;i++) {
			CHECK_ABORT // 休眠期间也需定期检查是否需要退出
			sleep(1); // 每次休眠1秒，分5次完成总5秒的休眠
		}
	}

	return NULL;
}

// 处理 SIGHUP 信号（通常与终端挂断相关），其核心功能是触发日志服务重新打开日志文件（例如用于日志轮转场景），通过向日志服务发送系统消息实现。
static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg; // 创建系统消息结构体
	smsg.source = 0; // 消息源：0 表示来自系统（非具体服务）
	smsg.session = 0; // 会话标识：0 表示无特定会话（系统消息无需会话关联）
	smsg.data = NULL; // 消息数据：无具体数据（仅通过类型标识操作）

	// 消息大小字段：通过移位操作设置消息类型为系统消息（PTYPE_SYSTEM）
	// MESSAGE_TYPE_SHIFT 是类型位移量，用于从 sz 字段中解析消息类型
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;

	// 查找名为 "logger" 的服务句柄（日志服务的唯一标识）
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		// 若找到日志服务，将消息推入其消息队列
		skynet_context_push(logger, &smsg);
	}
}

// 定时器线程入口函数，负责维护系统时间、驱动定时事件，并协调工作线程的唤醒，同时处理 SIGHUP 信号触发的日志重新打开操作
static void *
thread_timer(void *p) {
	struct monitor * m = p; // 接收监控器结构体指针，用于线程同步
	skynet_initthread(THREAD_TIMER);  // 初始化线程属性（标记为定时器线程）
	for (;;) { // 无限循环，持续提供定时服务
		skynet_updatetime(); // 更新系统当前时间（供框架内定时逻辑使用）
		skynet_socket_updatetime();  // 更新网络模块的时间（用于超时检测等）
		CHECK_ABORT // 检查是否所有服务都已退出，若则跳出循环
		wakeup(m,m->count-1); // 唤醒工作线程处理任务
		usleep(2500); // 休眠2.5毫秒，控制定时轮询频率
		if (SIG) { // 若收到SIGHUP信号（SIG被置1）
			signal_hup(); // 触发日志服务重新打开日志文件
			SIG = 0;  // 重置信号标记
		}
	}

	// 循环退出后执行清理操作（框架退出阶段）
	// wakeup socket thread
	skynet_socket_exit(); // 通知网络线程退出
	// wakeup all worker thread
	// 唤醒所有工作线程，使其退出循环
	pthread_mutex_lock(&m->mutex); // 加锁保护共享变量
	m->quit = 1; // 设置退出标记
	pthread_cond_broadcast(&m->cond); // 广播唤醒所有休眠的工作线程
	pthread_mutex_unlock(&m->mutex); // 解锁
	return NULL;
}

// 工作线程的入口函数，负责处理消息队列中的任务（如服务间消息、系统事件等）
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p; // 接收工作线程参数结构体（包含监控器、线程ID、权重等）
	int id = wp->id; // 工作线程唯一标识（用于关联对应的监控实例）
	int weight = wp->weight; // 线程权重（影响消息处理优先级或调度策略）
	struct monitor *m = wp->m; // 指向全局监控器（用于线程同步和状态管理）
	struct skynet_monitor *sm = m->m[id];  // 当前工作线程对应的监控实例（用于状态监控）
	skynet_initthread(THREAD_WORKER); // 初始化线程属性（标记为工作线程）
	struct message_queue * q = NULL; // 消息队列指针（用于次处理的消息队列）
	while (!m->quit) { // 循环处理消息，直到收到退出信号
		// 从消息队列中取出消息并调度处理，返回下一个待处理的消息队列（可能为NULL）
		q = skynet_context_message_dispatch(sm, q, weight); 
		if (q == NULL) { // 若没有可处理的消息队列
			if (pthread_mutex_lock(&m->mutex) == 0) { // 加锁进入临界区
				++ m->sleep; // 增加休眠线程计数
				// 若未收到退出信号，则等待条件变量唤醒（释放锁并阻塞）
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);  // 等待唤醒
				-- m->sleep; // 唤醒后减少休眠线程计数
				if (pthread_mutex_unlock(&m->mutex)) { // 解锁，若失败则退出程序
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

// 线程管理的核心函数，负责初始化线程监控器、创建并启动所有核心工作线程（包括监控线程、定时器线程、网络线程和业务工作线程），并在所有线程退出后清理资源
static void
start(int thread) {
	pthread_t pid[thread+3]; // 存储线程ID：thread个工作线程 + 3个辅助线程（监控、定时器、网络）

	// 初始化监控器（管理线程同步与状态）
	struct monitor *m = skynet_malloc(sizeof(*m)); 
	memset(m, 0, sizeof(*m)); // 初始化内存为0
	m->count = thread; // 记录工作线程总数
	m->sleep = 0; // 记录休眠的工作线程数

	// 为每个工作线程创建对应的监控实例
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new(); // 初始化单个线程监控器（用于检测线程异常）
	}

	// 初始化线程同步工具（互斥锁和条件变量）
	if (pthread_mutex_init(&m->mutex, NULL)) { // 初始化互斥锁（保护共享状态如sleep、quit）
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) { // 初始化条件变量（用于线程唤醒）
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 创建3个核心辅助线程
	create_thread(&pid[0], thread_monitor, m); // 监控线程：检测工作线程是否异常
	create_thread(&pid[1], thread_timer, m); // 定时器线程：处理定时任务、更新系统时间
	create_thread(&pid[2], thread_socket, m); // 网络线程：处理网络IO事件

	// 定义工作线程权重数组（影响消息处理优先级）
	static int weight[] = {
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, };
	// 创建业务工作线程
	struct worker_parm wp[thread]; // 工作线程参数数组
	for (i=0;i<thread;i++) {
		wp[i].m = m; // 关联监控器
		wp[i].id = i;  // 线程唯一ID（对应监控实例索引）
		// 分配权重：前32个线程使用预定义权重，超出部分默认0
		// 创建 worker 线程
    // 每次处理的工作量权重，是服务队列中消息总数右移的位数，小于 0 的每次只读一条
    // 前四个线程每次只处理一条消息
    // 后面的四个每次处理队列中的全部消息
    // 再后面分别是每次 1/2，1/4，1/8
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		// 创建工作线程
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	// 等待所有线程退出（阻塞主线程）
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); // 回收线程资源
	}

	// 清理监控器资源
	free_monitor(m);
}

// 业务入口启动器，负责解析用户配置的启动命令行（cmdline），解析出第一个服务名称和参数，启动该核心服务，并在启动失败时通过日志服务记录错误并退出框架
static void
bootstrap(uint32_t logger_handle, const char * cmdline) {
	// 获取命令行字符串长度
	int sz = strlen(cmdline);
	// 存储服务名称和参数分配缓冲区（+1 预留字符串结束符'\0'位置）
	char name[sz+1];
	char args[sz+1];
	int arg_pos; // 记录参数在命令行中的起始位置

	// 从命令行中解析出服务名称（以空格为分隔符的第一个字段）
	sscanf(cmdline, "%s", name);
	// 计算服务名称的长度（用于定位参数起始位置）
	arg_pos = strlen(name);

	// 解析命令行中的参数部分
	if (arg_pos < sz) {
		// 跳过服务名称后的空格（处理连续空格的情况）
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		// 将剩余部分复制到参数缓冲区（从第一个非空格字符开始）
		strncpy(args, cmdline + arg_pos, sz);
	} else { // 如果命令行只有服务名称，无参数
		args[0] = '\0'; // 参数设为空字符串
	}

	// 启动解析出的服务（name为服务名，args为参数）
	const uint32_t handle = skynet_context_new(name, args); // 启动服务
	if (handle == 0) { // 服务启动失败（返回句柄为0）
		// 获取日志服务的上下文（用于输出错误信息）
		struct skynet_context *logger = skynet_handle_grab(logger_handle);
		if (logger != NULL) {
			// 输出启动失败的错误信息（包含命令行内容）
			skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
			// 强制日志服务处理所有积压的消息（确保错误信息被输出）
			skynet_context_dispatchall(logger);
			// 释放日志服务的上下文引用
			skynet_context_release(logger);
		}
		// 启动失败，框架退出
		exit(1);
	}
}
// skynet_context_new启动服务的函数

// 入口函数，负责初始化框架核心组件、启动关键服务（如日志服务）、加载启动配置，并最终启动所有工作线程，完成框架的整体启动流程
void
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	// 注册 SIGHUP 信号处理器（用于日志文件重新打开）
	struct sigaction sa;
	sa.sa_handler = &handle_hup; // 绑定信号处理函数
	sa.sa_flags = SA_RESTART; // 系统调用被信号中断后自动重启
	sigfillset(&sa.sa_mask); // 信号处理期间屏蔽所有其他信号
	sigaction(SIGHUP, &sa, NULL);  // 注册 SIGHUP 信号的处理行为

	// 若配置为守护进程模式，则初始化守护进程
	if (config->daemon) {
		if (daemon_init(config->daemon)) { // 初始化守护进程（如fork子进程、脱离终端）
			exit(1);
		}
	}

	// 初始化框架核心组件
	skynet_harbor_init(config->harbor); // 初始化集群节点（多节点通信）
	skynet_handle_init(config->harbor); // 初始化服务句柄管理器（服务唯一标识）
	skynet_mq_init(); // 初始化服务句柄管理器（服务唯一标识）
	skynet_module_init(config->module_path);  // 初始化模块加载器（加载动态链接库）
	skynet_timer_init();  // 初始化定时器系统
	skynet_socket_init(); // 初始化网络 socket 模块
	skynet_profile_enable(config->profile); // 启用性能分析（若配置开启）

	// 启动日志服务
	const uint32_t logger_handle = skynet_context_new(config->logservice, config->logger);
	if (logger_handle == 0) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	// 为日志服务注册名称"logger"（便于通过名称查找服务）
	skynet_handle_namehandle(logger_handle, "logger");
	// 启动 bootstrap 服务（框架入口服务，通常是配置的第一个业务服务）
	bootstrap(logger_handle, config->bootstrap);
	// 启动所有工作线程、监控线程、定时器线程、网络线程
	start(config->thread);
	
	// 框架退出阶段：清理资源
	// 注意：harbor 退出可能涉及 socket 发送，需在 socket 释放前执行
	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit(); // 集群节点退出清理
	skynet_socket_free(); // 网络模块资源释放
	if (config->daemon) { // 若为守护进程模式，执行守护进程退出清理
		daemon_exit(config->daemon);
	}
}
