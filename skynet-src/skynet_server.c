#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) assert(__sync_lock_test_and_set(&ctx->calling,1) == 0);
#define CHECKCALLING_END(ctx) __sync_lock_release(&ctx->calling);
#define CHECKCALLING_INIT(ctx) ctx->calling = 0;
#define CHECKCALLING_DECL int calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DECL

#endif

struct skynet_context {
	void * instance; // 实例化
	struct skynet_module * mod; // 模块的指针
	uint32_t handle; // 句柄
	int ref;
	char result[32];
	void * cb_ud;
	skynet_cb cb; // 模块的返回函数
	int session_id; // 会话编号
	struct message_queue *queue; // 消息队列
	bool init; // 是否成功实例化 ‘_init’ 函数
	bool endless;

	CHECKCALLING_DECL
};

struct skynet_node {
	int total;
	uint32_t monitor_exit;
};

static struct skynet_node G_NODE = { 0,0 };
static __thread uint32_t handle_tls = 0xffffffff;

// 获得 Context 总数
int 
skynet_context_total() {
	return G_NODE.total;
}

// Context数 +1
static void
_context_inc() {
	__sync_fetch_and_add(&G_NODE.total,1);
}

// Context数 -1
static void
_context_dec() {
	__sync_fetch_and_sub(&G_NODE.total,1);
}

// 获得当前的句柄
uint32_t 
skynet_current_handle(void) {
	return handle_tls;
}

// 编号转十六进制字符串
static void
_id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

// 新建 Context，加载服务模块
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
        // 查询模块数组，找到则直接返回模块结构的指针
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL) // 没找到，则直接返回
		return NULL;

	void *inst = skynet_module_instance_create(mod); // 实例化 '_create' 函数
	if (inst == NULL) // 实例化失败，则直接返回
		return NULL;
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx)); // 分配内存
	CHECKCALLING_INIT(ctx)

	ctx->mod = mod; // 模块结构的指针
	ctx->instance = inst; // 实例化 '_create' 函数的指针
	ctx->ref = 2;
	ctx->cb = NULL; // 返回函数
	ctx->cb_ud = NULL;
	ctx->session_id = 0; // 会话编号

	ctx->init = false;
	ctx->endless = false;
	ctx->handle = skynet_handle_register(ctx);

	// 创建 Context 结构中的消息队列
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);
	// init function maybe use ctx->handle, so it must init at last
	_context_inc(); // Context数 +1

	CHECKCALLING_BEGIN(ctx)
	int r = skynet_module_instance_init(mod, inst, ctx, param); // 实例化 '_init' 函数
	CHECKCALLING_END(ctx)
	if (r == 0) {
		struct skynet_context * ret = skynet_context_release(ctx); // 实例化 '_release' 函数
		if (ret) {
			ctx->init = true; // 实例化 '_init' 成功
		}
		skynet_mq_force_push(queue); // 强行压入消息队列
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret; // 返回 ‘_release’ 函数的指针
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		skynet_context_release(ctx); // 释放 Context 结构
		skynet_handle_retire(ctx->handle);
		skynet_mq_release(queue); // 释放 消息队列
		return NULL; // 返回空值
	}
}

// 新会话
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number 会话永远是整数
	int session = (++ctx->session_id) & 0x7fffffff;
	return session;
}

void 
skynet_context_grab(struct skynet_context *ctx) {
	__sync_add_and_fetch(&ctx->ref,1); // 先加再返回
}

// 删除 Context 结构
static void 
_delete_context(struct skynet_context *ctx) {
	skynet_module_instance_release(ctx->mod, ctx->instance); // 执行模块中的 '_release' 函数
	skynet_mq_mark_release(ctx->queue); // 标记消息队列为释放状态
	skynet_free(ctx); // 释放 Context 结构
	_context_dec(); // Context 数 -1
}

// 释放 Context 结构
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	if (__sync_sub_and_fetch(&ctx->ref,1) == 0) {
		_delete_context(ctx); // 删除 Context 结构
		return NULL; // 返回空
	}
	return ctx; // 返回结构
}

// 压入 Context 结构到消息队列
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return -1;
	}
	skynet_mq_push(ctx->queue, message); // 压入消息队列
	skynet_context_release(ctx); // 释放 Context 结构

	return 0;
}

void 
skynet_context_endless(uint32_t handle) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	ctx->endless = true;
	skynet_context_release(ctx);
}

int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	int ret = skynet_harbor_message_isremote(handle);
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}

// 消息调度
static void
_dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	assert(ctx->init); // 断言
	CHECKCALLING_BEGIN(ctx)
	handle_tls = ctx->handle;
	int type = msg->sz >> HANDLE_REMOTE_SHIFT;
	size_t sz = msg->sz & HANDLE_MASK;

	// 执行服务模块中的返回函数
	if (!ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz)) {
		skynet_free(msg->data); // 释放数据
	}
	handle_tls = 0xffffffff;
	CHECKCALLING_END(ctx)
}

// 调度 Context 消息
int
skynet_context_message_dispatch(struct skynet_monitor *sm) {
	struct message_queue * q = skynet_globalmq_pop(); // 从全局队列中弹出消息队列
	if (q==NULL) // 如果为空
		return 1; // 返回 1

	uint32_t handle = skynet_mq_handle(q); // 获得消息队列的句柄

	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		int s = skynet_mq_release(q); // 释放消息队列
		if (s>0) {
			skynet_error(NULL, "Drop message queue %x (%d messages)", handle,s);
		}
		return 0;
	}

	struct skynet_message msg;
	if (skynet_mq_pop(q,&msg)) { // 弹出消息队列
		skynet_context_release(ctx); // 释放 Context 结构
		return 0;
	}

	skynet_monitor_trigger(sm, msg.source , handle); // 触发监视

	if (ctx->cb == NULL) { // 模块的返回函数为空
		skynet_free(msg.data); // 释放数据
		skynet_error(NULL, "Drop message from %x to %x without callback , size = %d",msg.source, handle, (int)msg.sz);
	} else {
		_dispatch_message(ctx, &msg); // 调度消息
	}

	assert(q == ctx->queue);
	skynet_mq_pushglobal(q); // 压入全局消息队列
	skynet_context_release(ctx); // 释放 Context 结构

	skynet_monitor_trigger(sm, 0,0); // 触发监视

	return 0;
}

// 复制名称
static void
_copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) { // 循环每个字符
		name[i] = addr[i]; // 将 addr 复制到 name 中
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0'; // 字符串最后 添加 '\0'
	}
}

// 查询名字
uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) { // 取名字的地一个字符
	case ':': // 如果为 ':' 冒号，表示为十六进制编号
		return strtoul(name+1,NULL,16);
	case '.': // 如果为 '.' 点号，表示为名称
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

// 句柄退出
static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) { // 如果句柄为0
		handle = context->handle;
		skynet_error(context, "KILL self"); // 杀死的是自己
	} else {
		skynet_error(context, "KILL :%0x", handle); // 杀死的是别人
	}
	if (G_NODE.monitor_exit) {
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	skynet_handle_retire(handle);
}

// Skynet 命令
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
        // 超时
        if (strcmp(cmd,"TIMEOUT") == 0) {
		char * session_ptr = NULL;
		int ti = strtol(param, &session_ptr, 10);
		int session = skynet_context_newsession(context);
		skynet_timeout(context->handle, ti, session);
		sprintf(context->result, "%d", session);
		return context->result;
	}

        // 锁住服务模块的消息队列
	if (strcmp(cmd,"LOCK") == 0) {
		if (context->init == false) {
			return NULL;
		}
		skynet_mq_lock(context->queue, context->session_id+1);
		return NULL;
	}

	// 解锁服务模块的消息队列
	if (strcmp(cmd,"UNLOCK") == 0) {
		if (context->init == false) {
			return NULL;
		}
		skynet_mq_unlock(context->queue);
		return NULL;
	}

	// 给服务注册一个名字
	if (strcmp(cmd,"REG") == 0) {
		if (param == NULL || param[0] == '\0') {
			sprintf(context->result, ":%x", context->handle);
			return context->result;
		} else if (param[0] == '.') {
			return skynet_handle_namehandle(context->handle, param + 1);
		} else {
			assert(context->handle!=0);
			struct remote_name *rname = skynet_malloc(sizeof(*rname));
			_copy_name(rname->name, param);
			rname->handle = context->handle;
			skynet_harbor_register(rname);
			return NULL;
		}
	}

	// 根据名字查找服务
	if (strcmp(cmd,"QUERY") == 0) {
		if (param[0] == '.') {
			uint32_t handle = skynet_handle_findname(param+1);
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
		return NULL;
	}

	// 获得名字
	if (strcmp(cmd,"NAME") == 0) {
		int size = strlen(param);
		char name[size+1];
		char handle[size+1];
		sscanf(param,"%s %s",name,handle);
		if (handle[0] != ':') {
			return NULL;
		}
		uint32_t handle_id = strtoul(handle+1, NULL, 16);
		if (handle_id == 0) {
			return NULL;
		}
		if (name[0] == '.') {
			return skynet_handle_namehandle(handle_id, name + 1);
		} else {
			struct remote_name *rname = skynet_malloc(sizeof(*rname));
			_copy_name(rname->name, name);
			rname->handle = handle_id;
			skynet_harbor_register(rname);
		}
		return NULL;
	}

	// 获得当前时间
	if (strcmp(cmd,"NOW") == 0) {
		uint32_t ti = skynet_gettime();
		sprintf(context->result,"%u",ti);
		return context->result;
	}

	// 退出服务
	if (strcmp(cmd,"EXIT") == 0) {
		handle_exit(context, 0);
		return NULL;
	}

	// 杀死服务
	if (strcmp(cmd,"KILL") == 0) {
		uint32_t handle = 0;
		if (param[0] == ':') {
			handle = strtoul(param+1, NULL, 16);
		} else if (param[0] == '.') {
			handle = skynet_handle_findname(param+1);
		} else {
			skynet_error(context, "Can't kill %s",param);
			// todo : kill global service
		}
		if (handle) {
			handle_exit(context, handle);
		}
		return NULL;
	}

	// 加载服务
	if (strcmp(cmd,"LAUNCH") == 0) {
		size_t sz = strlen(param);
		char tmp[sz+1];
		strcpy(tmp,param);
		char * args = tmp;
		char * mod = strsep(&args, " \t\r\n");
		args = strsep(&args, "\r\n");
		struct skynet_context * inst = skynet_context_new(mod,args);
		if (inst == NULL) {
			return NULL;
		} else {
			_id_to_hex(context->result, inst->handle);
			return context->result;
		}
	}

	// 获得 LUA 环境变量
	if (strcmp(cmd,"GETENV") == 0) {
		return skynet_getenv(param);
	}

	// 设置 LUA 环境变量
	if (strcmp(cmd,"SETENV") == 0) {
		size_t sz = strlen(param);
		char key[sz+1];
		int i;
		for (i=0;param[i] != ' ' && param[i];i++) {
			key[i] = param[i];
		}
		if (param[i] == '\0')
			return NULL;

		key[i] = '\0';
		param += i+1;
		
		skynet_setenv(key,param);
		return NULL;
	}

	// 获得启动时间
	if (strcmp(cmd,"STARTTIME") == 0) {
		uint32_t sec = skynet_gettime_fixsec();
		sprintf(context->result,"%u",sec);
		return context->result;
	}

	// 获得是否已经释放的标志
	if (strcmp(cmd,"ENDLESS") == 0) {
		if (context->endless) {
			strcpy(context->result, "1");
			context->endless = false;
			return context->result;
		}
		return NULL;
	}

	// 终端所有服务
	if (strcmp(cmd,"ABORT") == 0) {
		skynet_handle_retireall();
		return NULL;
	}

	// 监视
	if (strcmp(cmd,"MONITOR") == 0) {
		uint32_t handle=0;
		if (param == NULL || param[0] == '\0') {
			if (G_NODE.monitor_exit) {
				// return current monitor serivce
				sprintf(context->result, ":%x", G_NODE.monitor_exit);
				return context->result;
			}
			return NULL;
		} else {
			if (param[0] == ':') {
				handle = strtoul(param+1, NULL, 16);
			} else if (param[0] == '.') {
				handle = skynet_handle_findname(param+1);
			} else {
				skynet_error(context, "Can't monitor %s",param);
				// todo : monitor global service
			}
		}
		G_NODE.monitor_exit = handle;
		return NULL;
	}

	// 获得消息队列的长度
	if (strcmp(cmd, "MQLEN") == 0) {
		int len = skynet_mq_length(context->queue);
		sprintf(context->result, "%d", len);
		return context->result;
	}

	return NULL;
}

static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;
	type &= 0xff;

	if (allocsession) {
		assert(*session == 0);
		*session = skynet_context_newsession(context);
	}

	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	assert((*sz & HANDLE_MASK) == *sz);
	*sz |= type << HANDLE_REMOTE_SHIFT;
}

// 发送消息给服务
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	_filter_args(context, type, &session, (void **)&data, &sz);

	if (source == 0) {
		source = context->handle;
	}

	if (destination == 0) {
		return session;
	}
	if (skynet_harbor_message_isremote(destination)) {
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		skynet_harbor_send(rmsg, source, session);
	} else {
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			skynet_error(NULL, "Drop message from %x to %x (type=%d)(size=%d)", source, destination, type&0xff, (int)(sz & HANDLE_MASK));
			return -1;
		}
	}
	return session;
}

// 根据名称发生消息给服务
int
skynet_sendname(struct skynet_context * context, const char * addr , int type, int session, void * data, size_t sz) {
	uint32_t source = context->handle;
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtoul(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
  			skynet_free(data);
  		}
			skynet_error(context, "Drop message to %s", addr);
			return session;
		}
	} else {
		_filter_args(context, type, &session, (void **)&data, &sz);

		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		_copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}

	return skynet_send(context, source, des, type, session, data, sz);
}

// 获得 Context 句柄
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

// Context 初始化
void 
skynet_context_init(struct skynet_context *ctx, uint32_t handle) {
	ctx->handle = handle;
}

// 设置服务模块的返回函数
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}

void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | type << HANDLE_REMOTE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);
}
