#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

#include <string.h>

// 框架的核心参数配置，框架启动和初始化的关键数据结构
struct skynet_config {
	int thread; // 工作线程数量， 总线程数量 = thread + THREAD_MAIN + THREAD_SOCKET + THREAD_TIMER + THREAD_MONITOR
	int harbor; // 集群节点标识。每个节点需要一个唯一的harbor值，通常为非负整数
	int profile; // 性能分析开关，0表示关闭，1表示开启
	const char * daemon; // 守护进程模式配置
	const char * module_path; // 搜索模块路径
	const char * bootstrap; // 启动脚本路径
	const char * logger; // 日志输出配置
	const char * logservice; // 日志服务类型
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

void skynet_start(struct skynet_config * config);

// 静态内联函数，返回复制后的字符串指针，参数为源字符串和需要复制的长度
static inline char *
skynet_strndup(const char *str, size_t size) {
	// 分配内存：大小为指定长度 + 1（额外1字节用于存储字符串结束符'\0'）
	char * ret = skynet_malloc(size+1);
	// 内存分配失败时返回NULL
	if (ret == NULL) return NULL;
	// 将源字符串中前size个字节复制到新分配的内存中
	memcpy(ret, str, size);
	// 在复制内容的末尾添加字符串结束符，确保字符串格式正确
	ret[size] = '\0';
	// 返回复制后的字符串指针
	return ret;
}

static inline char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str); // 计算源字符串的长度（不包含结束符'\0'）
	return skynet_strndup(str, sz); // 调用skynet_strndup复制指定长度的字符串
}

#endif
