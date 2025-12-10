#include "skynet.h"

#include "skynet_socket.h"
#include "socket_server.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_harbor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static struct socket_server * SOCKET_SERVER = NULL; // 唯一的 socket 服务器实例

// 创建并初始化底层的 socket 服务器实例，为框架的网络通信功能提供基础支持
void 
skynet_socket_init() {
	SOCKET_SERVER = socket_server_create(skynet_now());
}

void
skynet_socket_exit() {
	socket_server_exit(SOCKET_SERVER);
}

void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

void
skynet_socket_updatetime() {
	socket_server_updatetime(SOCKET_SERVER, skynet_now());
}

// mainloop thread
static void
forward_message(int type, bool padding, struct socket_message * result) {
	// 1. 变量初始化与内存大小计算
	struct skynet_socket_message *sm; 
	size_t sz = sizeof(*sm);  // 基础消息结构大小
	if (padding) { // 需要额外存储字符串数据（如连接地址、错误信息）
		if (result->data) {
			size_t msg_sz = strlen(result->data);
			if (msg_sz > 128) {
				msg_sz = 128; // 限制最大长度为 128 字节，避免消息过大
			}
			sz += msg_sz;  // 总大小 = 基础结构大小 + 字符串长度
		} else {
			result->data = ""; // 若数据为空，默认赋值空字符串
		}
	}
	// 2. 分配消息内存并填充内容
	sm = (struct skynet_socket_message *)skynet_malloc(sz);  // 分配总内存
	sm->type = type; // 框架定义的事件类型（如 SKYNET_SOCKET_TYPE_ACCEPT）
	sm->id = result->id; // socket 连接的唯一标识 ID
	sm->ud = result->ud; // 附加数据（如错误码、端口号等）
	if (padding) {
		sm->buffer = NULL; // 字符串数据已嵌入消息内存，无需单独指针
		// 将 result->data 复制到 sm 结构体后面的内存空间（柔性数组思想）
		memcpy(sm+1, result->data, sz - sizeof(*sm));
	} else {
		sm->buffer = result->data; // 直接指向底层数据（如网络数据包，无需复制）
	}

	// 3. 封装为框架消息并推送
	struct skynet_message message;
	message.source = 0;
	message.session = 0;
	message.data = sm;

	// 消息大小 | 消息类型（PTYPE_SOCKET 表示是 socket 事件消息）
	message.sz = sz | ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);

	// 推送到目标上下文（result->opaque 为目标上下文的 handle）
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// todo: report somewhere to close socket
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}

// 获取就绪事件并转发给对应业务模块
int 
skynet_socket_poll() {
	// 1.获取 socket 服务实例并校验
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss); // // 确保 socket 服务已初始化，否则触发断言失败

	// 2. 获取就绪事件
	struct socket_message result; // 存储从 socket 服务获取的事件详情
	int more = 1;  // 标记是否还有更多未处理的事件（输出参数）
	int type = socket_server_poll(ss, &result, &more); // 从 socket 服务中获取一个就绪事件，返回事件类型

	// 3. 根据事件类型转发消息
	switch (type) {
	case SOCKET_EXIT: // socket 服务退出事件
		return 0; // 告知框架 socket 服务已退出
	case SOCKET_DATA: // 数据到达事件
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result); // 转发消息
		break;
	case SOCKET_CLOSE: // 关闭，转发socket关闭消息
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;
	case SOCKET_OPEN: // 连接建立，转发
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;
	case SOCKET_ERR: // 错误
		forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
		break;
	case SOCKET_ACCEPT: //  SOCKET_ACCEPT 表示新连接接入
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;
	case SOCKET_UDP: // udp的消息
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	case SOCKET_WARNING:
		forward_message(SKYNET_SOCKET_TYPE_WARNING, false, &result);
		break;
	default:
		skynet_error(NULL, "error: Unknown socket message type %d.",type);
		return -1;
	}
	if (more) { // 还有数据没处理完，告知外部循环接着poll
		return -1;
	}
	return 1;
}

int
skynet_socket_sendbuffer(struct skynet_context *ctx, struct socket_sendbuffer *buffer) {
	return socket_server_send(SOCKET_SERVER, buffer);
}

int
skynet_socket_sendbuffer_lowpriority(struct skynet_context *ctx, struct socket_sendbuffer *buffer) {
	return socket_server_send_lowpriority(SOCKET_SERVER, buffer);
}

int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

int 
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_bind(SOCKET_SERVER, source, fd);
}

void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

void 
skynet_socket_shutdown(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_shutdown(SOCKET_SERVER, source, id);
}

void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}

void
skynet_socket_pause(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_pause(SOCKET_SERVER, source, id);
}


void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}

int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}

int
skynet_socket_udp_dial(struct skynet_context *ctx, const char * addr, int port){
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp_dial(SOCKET_SERVER, source, addr, port);
}

int
skynet_socket_udp_listen(struct skynet_context *ctx, const char * addr, int port){
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp_listen(SOCKET_SERVER, source, addr, port);
}

int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}

int 
skynet_socket_udp_sendbuffer(struct skynet_context *ctx, const char * address, struct socket_sendbuffer *buffer) {
	return socket_server_udp_send(SOCKET_SERVER, (const struct socket_udp_address *)address, buffer);
}

// 从 UDP 类型的 socket 事件消息中获取发送方的地址信息
const char *
skynet_socket_udp_address(struct skynet_socket_message *msg, int *addrsz) {
	// 1. UDP 消息类型校验
	if (msg->type != SKYNET_SOCKET_TYPE_UDP) {
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;  // 复制 socket 连接的唯一标识 ID
	sm.opaque = 0;    // 透明字段（此处无需使用，设为 0）
	sm.ud = msg->ud;  // 复制附加数据（可能包含地址相关的元信息）
	sm.data = msg->buffer; // 复制数据缓冲区（UDP 数据包内容）
	return (const char *)socket_server_udp_address(SOCKET_SERVER, &sm, addrsz); // 调用底层函数获取地址
}

struct socket_info *
skynet_socket_info() {
	return socket_server_info(SOCKET_SERVER);
}
