#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>

// 监控消息处理状态
struct skynet_monitor {
	ATOM_INT version; // 记录监控状态的更新次数,线程安全
	int check_version; // 用于检查的版本号
	uint32_t source; // 存储消息发送方的服务 ID（Skynet 中用 32 位整数标识服务）。
	uint32_t destination; // 存储消息接收方的服务 ID。
};

// 创建一个monitor结构
struct skynet_monitor * 
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret)); // 分配内存
	memset(ret, 0, sizeof(*ret)); // 初始化内存
	return ret;
}

// 删除一个monitor结构
void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}

// 触发监控状态更新，记录消息的发送方和接收方，并更新版本号
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	// 记录消息发送方的服务 ID（source 为 32 位无符号整数，标识发送服务）
	sm->source = source;
	// 记录消息接收方的服务 ID（destination 为 32 位无符号整数，标识接收服务）
	sm->destination = destination;
	// 原子操作自增版本号（ATOM_FINC 确保多线程环境下版本号递增的线程安全）
	// 版本号更新用于标识监控状态发生了变化
	ATOM_FINC(&sm->version);
}

void 
skynet_monitor_check(struct skynet_monitor *sm) {
	 // 对比当前版本号与上一次检查的版本号
	if (sm->version == sm->check_version) {
		// 若版本号未变化，且存在消息接收方（destination 不为 0）
		if (sm->destination) {
			// 标记该接收方服务为"无限循环状态"（可能触发后续处理，如终止服务）
			skynet_context_endless(sm->destination);
			// 输出错误日志，提示可能存在无限循环，包含发送方、接收方服务ID及版本号
			skynet_error(NULL, "error: A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		// 若版本号已更新，说明消息处理正常，更新检查版本号为当前版本
		sm->check_version = sm->version;
	}
}
