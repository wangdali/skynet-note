#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

static struct skynet_context * REMOTE = 0; // 远程节点的 Context 结构指针
static unsigned int HARBOR = 0; // 节点的全局变量

// 节点发送
void 
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
	int type = rmsg->sz >> HANDLE_REMOTE_SHIFT;
	rmsg->sz &= HANDLE_MASK;
	assert(type != PTYPE_SYSTEM && type != PTYPE_HARBOR); // 断言

	// 发送消息
	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg) , source, type , session);
}

// 节点注册
void 
skynet_harbor_register(struct remote_name *rname) {
	int i;
	int number = 1;
	for (i=0;i<GLOBALNAME_LENGTH;i++) {
		char c = rname->name[i];
		if (!(c >= '0' && c <='9')) {
			number = 0;
			break;
		}
	}
	assert(number == 0); // 断言

	// 发送消息
	skynet_context_send(REMOTE, rname, sizeof(*rname), 0, PTYPE_SYSTEM , 0);
}

// 节点消息是否为远程
int 
skynet_harbor_message_isremote(uint32_t handle) {
	int h = (handle & ~HANDLE_MASK);
	return h != HARBOR && h !=0;
}

// 初始化节点
void
skynet_harbor_init(int harbor) {
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT; // 左移24位，设置节点编号
}

// 启动节点
int
skynet_harbor_start(const char * master, const char *local) {
	size_t sz = strlen(master) + strlen(local) + 32; // 计算字符串长度
	char args[sz]; // 参数字符串

	// 将master地址、节点地址和节点编号组成参数字符串
	sprintf(args, "%s %s %d",master,local,HARBOR >> HANDLE_REMOTE_SHIFT);
	struct skynet_context * inst = skynet_context_new("harbor",args); // 加载新的服务模块 ‘harbor.so’
	if (inst == NULL) { // 加载失败
		return 1; // 返回 1
	}
	REMOTE = inst; // 加载成功，设置远程 Context 结构的指针

	return 0; // 返回 0
}
