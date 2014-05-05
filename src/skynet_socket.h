#ifndef skynet_socket_h
#define skynet_socket_h

struct skynet_context;

#define SKYNET_SOCKET_TYPE_DATA 1
#define SKYNET_SOCKET_TYPE_CONNECT 2
#define SKYNET_SOCKET_TYPE_CLOSE 3
#define SKYNET_SOCKET_TYPE_ACCEPT 4
#define SKYNET_SOCKET_TYPE_ERROR 5

struct skynet_socket_message {
	int type; // 类型
	int id; // 编号
	int ud;
	char * buffer; // 缓冲区
};

void skynet_socket_init(); // 初始化 Socket
void skynet_socket_exit(); // 退出 Socket
void skynet_socket_free(); // 释放 Socket
int skynet_socket_poll();  // 查看 Socket 消息

int skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz); // 发送数据
void skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz); // 低优先级发送数据
int skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog); // 监听 Socket
int skynet_socket_connect(struct skynet_context *ctx, const char *host, int port); // Socket 连接
int skynet_socket_block_connect(struct skynet_context *ctx, const char *host, int port); // 阻塞式 Socket 连接
int skynet_socket_bind(struct skynet_context *ctx, int fd); // 绑定事件
void skynet_socket_close(struct skynet_context *ctx, int id); // 关闭 Socket
void skynet_socket_start(struct skynet_context *ctx, int id); // 启动 Socket

#endif
