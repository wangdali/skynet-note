#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct monitor {
	int count; // 线程总数
	struct skynet_monitor ** m;
	pthread_cond_t cond; // 线程条件变量
	pthread_mutex_t mutex; // 线程互斥锁
	int sleep; // 睡眠
};

struct worker_parm {
	struct monitor *m; // 监视结构
	int id; // 编号
};

// 检查是否中断
#define CHECK_ABORT if (skynet_context_total()==0) break; // 如果上下文总数为0

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) { // 创建线程
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond); // 通过条件变量唤醒线程
	}
}

static void *
_socket(void *p) {
	struct monitor * m = p;
	for (;;) {
		int r = skynet_socket_poll(); // Socket池
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT // 检查是否中断
			continue;
		}
		wakeup(m,0); // 唤醒线程
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]); // 删除 监视
	}
	pthread_mutex_destroy(&m->mutex); // 销毁互斥锁
	pthread_cond_destroy(&m->cond); // 销毁条件变量
	skynet_free(m->m); // 释放 监视结构中的结构数组
	skynet_free(m); // 释放监视结构
}

static void *
_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count; // 线程数
	for (;;) {
		CHECK_ABORT // 检查是否中断
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]); // 检查 监视
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT // 检查是否中断
			sleep(1); // 睡眠 1秒
		}
	}

	return NULL;
}

static void *
_timer(void *p) {
	struct monitor * m = p;
	for (;;) {
		skynet_updatetime(); // 更新 定时器 的时间
		CHECK_ABORT // 检查是否中断
		wakeup(m,m->count-1); // 唤醒线程
		usleep(2500); // 睡眠 2500 微妙（1秒=1000000微秒）
	}
	// wakeup socket thread
	skynet_socket_exit(); // 退出 Socket
	// wakeup all worker thread
	pthread_cond_broadcast(&m->cond); // 广播条件变量
	return NULL;
}

static void *
_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	for (;;) {
		if (skynet_context_message_dispatch(sm)) { // 调度Skynet的上下文消息
			CHECK_ABORT
			if (pthread_mutex_lock(&m->mutex) == 0) { // 加锁
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) { // 解锁
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		} 
	}
	return NULL;
}

static void
_start(int thread) {
	pthread_t pid[thread+3]; // 线程编号的数组

	struct monitor *m = skynet_malloc(sizeof(*m)); // 分配 监视 结构的内存
	memset(m, 0, sizeof(*m)); // 清空结构
	m->count = thread; // 线程总数
	m->sleep = 0; // 不睡眠

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new(); // 为每个线程新建一个监视
	}
	if (pthread_mutex_init(&m->mutex, NULL)) { // 初始化互斥锁
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) { // 初始化线程条件变量
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	create_thread(&pid[0], _monitor, m);    // 创建 监视 线程
	create_thread(&pid[1], _timer, m);      // 创建 定时器 线程
	create_thread(&pid[2], _socket, m);     // 创建 网络 线程

	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		create_thread(&pid[i+3], _worker, &wp[i]); // 创建多个工作线程
	}

	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	free_monitor(m); // 释放 监视
}

static int
_start_master(const char * master) {
	struct skynet_context *ctx = skynet_context_new("master", master); // 加载 master 服务
	if (ctx == NULL)
		return 1;
	return 0;	
}

void 
skynet_start(struct skynet_config * config) {
	skynet_harbor_init(config->harbor); // 初始化节点
	skynet_handle_init(config->harbor); // 初始化句柄
	skynet_mq_init(); // 初始化消息队列
	skynet_module_init(config->module_path); // 初始化模块
	skynet_timer_init(); // 初始化定时器
	skynet_socket_init(); // 初始化网络

	struct skynet_context *ctx;
	ctx = skynet_context_new("logger", config->logger); // 加载日志服务
	if (ctx == NULL) {
		fprintf(stderr,"launch logger error");
		exit(1);
	}

	// 如果设置了 master 监听地址
	if (config->standalone) {
		if (_start_master(config->standalone)) { // 启动 master 服务
			fprintf(stderr, "Init fail : mater");
			return;
		}
	}
	// harbor must be init first
	// 启动节点服务，必须先初始化。
	if (skynet_harbor_start(config->master , config->local)) {
		fprintf(stderr, "Init fail : no master");
		return;
	}

	// 加载 LUA加载服务
	ctx = skynet_context_new("snlua", "launcher");
	if (ctx) {
		skynet_command(ctx, "REG", ".launcher"); // 给服务注册名字
		ctx = skynet_context_new("snlua", config->start); // 启动第一个 LUA服务
	}

	_start(config->thread); // 开始
	skynet_socket_free(); // 释放网络
}

