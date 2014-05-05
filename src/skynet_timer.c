#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#if defined(__APPLE__) // 苹果平台
#include <sys/time.h>
#endif

typedef void (*timer_execute_func)(void *ud,void *arg); // 函数指针类型

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

// 定时器事件
struct timer_event {
	uint32_t handle; // 句柄
	int session; // 会话
};

// 定时器节点
struct timer_node {
	struct timer_node *next; // 下一个定时器节点
	int expire; // 到期时间
};

// 链表
struct link_list {
	struct timer_node head; // 链表头
	struct timer_node *tail; // 链表尾
};

// 定时器
struct timer {
	struct link_list near[TIME_NEAR];
	struct link_list t[4][TIME_LEVEL-1];
	int lock; // 锁
	int time; // 时间
	uint32_t current; //当前时间
	uint32_t starttime; // 启动时间
};

static struct timer * TI = NULL; // 全局定时器指针变量

// 清除链表
static inline struct timer_node *
link_clear(struct link_list *list)
{
	struct timer_node * ret = list->head.next; // 获得链表头的下一个节点
	list->head.next = 0; // 链表头的下一个节点为0
	list->tail = &(list->head); // 链表尾 = 链表头

	return ret; // 返回链表头的下一个节点
}

static inline void
link(struct link_list *list,struct timer_node *node)
{
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

// 添加节点
static void
add_node(struct timer *T,struct timer_node *node)
{
	int time=node->expire;
	int current_time=T->time;
	
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
	}
	else {
		int i;
		int mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)-1],node);	
	}
}

// 添加定时器
static void
timer_add(struct timer *T,void *arg,size_t sz,int time)
{
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);

	while (__sync_lock_test_and_set(&T->lock,1)) {};

		node->expire=time+T->time;
		add_node(T,node);

	__sync_lock_release(&T->lock);
}

static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;
	int time = (++T->time) >> TIME_NEAR_SHIFT;
	int i=0;
	
	while ((T->time & (mask-1))==0) {
		int idx=time & TIME_LEVEL_MASK;
		if (idx!=0) {
			--idx;
			struct timer_node *current = link_clear(&T->t[i][idx]);
			while (current) {
				struct timer_node *temp=current->next;
				add_node(T,current);
				current=temp;
			}
			break;				
		}
		mask <<= TIME_LEVEL_SHIFT;
		time >>= TIME_LEVEL_SHIFT;
		++i;
	}	
}

// 执行定时器
static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;
	
	while (T->near[idx].head.next) {
		struct timer_node *current = link_clear(&T->near[idx]);
		
		do {
			struct timer_event * event = (struct timer_event *)(current+1);
			struct skynet_message message;
			message.source = 0;
			message.session = event->session;
			message.data = NULL;
			message.sz = PTYPE_RESPONSE << HANDLE_REMOTE_SHIFT;

			skynet_context_push(event->handle, &message);
			
			struct timer_node * temp = current;
			current=current->next;
			skynet_free(temp);	
		} while (current);
	}
}

// 更新定时器
static void 
timer_update(struct timer *T)
{
	while (__sync_lock_test_and_set(&T->lock,1)) {};

	// try to dispatch timeout 0 (rare condition)
	timer_execute(T);

	// shift time first, and then dispatch timer message
	timer_shift(T);
	timer_execute(T);

	__sync_lock_release(&T->lock);
}

// 创建定时器
static struct timer *
timer_create_timer()
{
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer)); // 分配内存
	memset(r,0,sizeof(*r)); // 清空结构

	int i,j; // 声明变量

	for (i=0;i<TIME_NEAR;i++) { // TIME_NEAR: 1<<8
		link_clear(&r->near[i]); // 清除链表
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL-1;j++) { // TIME_LEVEL: 1<<6
			link_clear(&r->t[i][j]); // 清除链表
		}
	}

	r->lock = 0; // 锁
	r->current = 0; // 当前时间

	return r; // 返回定时器的结构
}

// 超时
int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time == 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = PTYPE_RESPONSE << HANDLE_REMOTE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// 获得时间
static uint32_t
_gettime(void) {
	uint32_t t;
#if !defined(__APPLE__) // 非苹果平台
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti); // 获得时间
	t = (uint32_t)(ti.tv_sec & 0xffffff) * 100; // 秒数 & 0xffffff * 100
	t += ti.tv_nsec / 10000000; // t += 纳秒数 / 10 000 000 (1秒=1 000 000 000纳秒)
#else // 苹果平台
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint32_t)(tv.tv_sec & 0xffffff) * 100;
	t += tv.tv_usec / 10000;
#endif
	return t; // 返回 t
}

// 更新时间
void
skynet_updatetime(void) {
	uint32_t ct = _gettime();
	if (ct != TI->current) {
		int diff = ct>=TI->current?ct-TI->current:(0xffffff+1)*100-TI->current+ct;
		TI->current = ct;
		int i;
		for (i=0;i<diff;i++) {
			timer_update(TI);
		}
	}
}

// 获得启动时间
uint32_t
skynet_gettime_fixsec(void) {
	return TI->starttime; // 启动时间
}

// 获得时间
uint32_t 
skynet_gettime(void) {
	return TI->current; // 当前时间
}

// 定时器初始化
void 
skynet_timer_init(void) {
	TI = timer_create_timer(); // 创建定时器
	TI->current = _gettime(); // 获得当前时间

#if !defined(__APPLE__) // 非苹果平台
	struct timespec ti; // tv_sec为秒; tv_nsec为纳秒
	clock_gettime(CLOCK_REALTIME, &ti); // 获得时间
	uint32_t sec = (uint32_t)ti.tv_sec; // 取秒数
#else // 苹果平台
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint32_t sec = (uint32_t)tv.tv_sec;
#endif
	uint32_t mono = _gettime() / 100; // 获得当前时间 / 100

	TI->starttime = sec - mono; // 计算启动时间
}
