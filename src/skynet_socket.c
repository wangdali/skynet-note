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

static struct socket_server * SOCKET_SERVER = NULL; // 全局变量

// 初始化 Socket
void 
skynet_socket_init() {
	SOCKET_SERVER = socket_server_create(); // 创建 Socket Server
}

// 退出 Socket
void
skynet_socket_exit() {
	socket_server_exit(SOCKET_SERVER); // 退出 Socket Server
}

// 释放 Socket
void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER); // 释放 Socket Server
	SOCKET_SERVER = NULL; // 设置全局变量为空
}

// mainloop thread
// 转发消息
static void
forward_message(int type, bool padding, struct socket_message * result) {
	struct skynet_socket_message *sm; // Socket 消息
	int sz = sizeof(*sm);
	if (padding) { // 判断是否正在填充状态
		if (result->data) { // 是否有数据
			sz += strlen(result->data) + 1; // 获得数据的长度 +1
		} else {
			result->data = ""; // 设置数据为空
			sz += 1; // 长度为 +1
		}
	}
	sm = (struct skynet_socket_message *)skynet_malloc(sz); // 分配内存
	sm->type = type; // Socket 消息的类型
	sm->id = result->id; // Socket 的编号
	sm->ud = result->ud;
	if (padding) { // 判断是否正在填充状态
		sm->buffer = NULL; // 设置缓冲为空
		strcpy((char*)(sm+1), result->data);
	} else {
		sm->buffer = result->data;
	}

	struct skynet_message message; // Skynet 消息
	message.source = 0; // 来源为 0
	message.session = 0; // 会话为 0
	message.data = sm; // 数据为 Socket 消息
	message.sz = sz | PTYPE_SOCKET << HANDLE_REMOTE_SHIFT; // 数据的长度
	
	// 将 Skynet 消息压入消息队列
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// todo: report somewhere to close socket，报告某处关闭了 Socket
	        // 这里不要调用 skynet_socket_close （它将阻塞主循环）
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm); //释放消息
	}
}

int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER; // 设为全局变量
	assert(ss); // 断言
	struct socket_message result; // Socket 消息
	int more = 1;
	int type = socket_server_poll(ss, &result, &more); // 查看 Socket 消息
	switch (type) {
	case SOCKET_EXIT: // 退出 Socket
		return 0;
	case SOCKET_DATA: // Socket 数据到来
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
		break;
	case SOCKET_CLOSE: // 关闭 Socket
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;
	case SOCKET_OPEN: // 打开 Socket
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;
	case SOCKET_ERROR: // Socket 错误
		forward_message(SKYNET_SOCKET_TYPE_ERROR, false, &result);
		break;
	case SOCKET_ACCEPT: // 接受新的 Socket
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;
	default: // 未知 Socket 消息类型
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}
	if (more) {
		return -1;
	}
	return 1;
}

// 发送 Socket 数据
int
skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	int64_t wsz = socket_server_send(SOCKET_SERVER, id, buffer, sz); // 发送数据
	if (wsz < 0) {
		skynet_free(buffer);
		return -1;
	} else if (wsz > 1024 * 1024) {
		int kb4 = wsz / 1024 / 4;
		if (kb4 % 256 == 0) {
			skynet_error(ctx, "%d Mb bytes on socket %d need to send out", (int)(wsz / (1024 * 1024)), id);
		}
	}
	return 0;
}

// 低优先级发送 Socket 数据
void
skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	socket_server_send_lowpriority(SOCKET_SERVER, id, buffer, sz);
}

// 监听 Socket
int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

// Socket 连接
int 
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

// 阻塞式 Socket 连接
int 
skynet_socket_block_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_block_connect(SOCKET_SERVER, source, host, port);
}

// 绑定事件
int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_bind(SOCKET_SERVER, source, fd);
}

// 关闭 Socket
void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

// 启动 Socket
void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}
