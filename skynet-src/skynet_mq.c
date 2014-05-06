///
/// \file skynet_mq.c
/// \brief 消息队列
///
#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64 ///< 默认队列大小
#define MAX_GLOBAL_MQ 0x10000 ///< 最大全局消息队列大小(64K)

// 0 means mq is not in global mq. 不是全局消息队列
// 1 means mq is in global mq , or the message is dispatching. 是全局消息队列或消息正在调度
// 2 means message is dispatching with locked session set. 消息正在调度，和设置会话为锁定。
// 3 means mq is not in global mq, and locked session has been set. 不是全局消息队列，和会话已设置为锁定。

#define MQ_IN_GLOBAL 1 ///< 在全局队列中
#define MQ_DISPATCHING 2 ///< 正在调度
#define MQ_LOCKED 3 ///< 已锁定

/// 消息队列的结构
struct message_queue {
	uint32_t handle; ///< 句柄
	int cap; ///< 队列大小
	int head; ///< 队列头
	int tail; ///< 队列尾
	int lock; ///< 队列锁
	int release; ///< 释放
	int lock_session; ///< 会话锁
	int in_global; ///< 全局
	struct skynet_message *queue; ///< 消息
};

/// 全局队列的结构
struct global_queue {
	uint32_t head; ///< 队列头
	uint32_t tail; ///< 队列尾
	struct message_queue ** queue; ///< 消息队列的指针的指针
	bool * flag; ///< 标记

};

static struct global_queue *Q = NULL; ///< 全局队列的指针变量

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {} ///< 加锁
#define UNLOCK(q) __sync_lock_release(&(q)->lock); ///< 解锁

#define GP(p) ((p) % MAX_GLOBAL_MQ) ///< 求余数

/// 压入全局消息队列
/// \param[in] *queue
/// \return static void
static void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q; // 全局队列

	// 如果tail > MAX_GLOBAL_MQ，将从头开始
	uint32_t tail = GP(__sync_fetch_and_add(&q->tail,1)); // 求余数
	q->queue[tail] = queue; // 将消息队列的结构放入全局队列的尾部
	__sync_synchronize(); // 同步，执行队列尾值+1
	q->flag[tail] = true; // 标志为真
}

/// 弹出全局消息队列
/// \return struct message_queue *
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q; // 全局队列
	uint32_t head =  q->head; // 获得全局队列头的值
	uint32_t head_ptr = GP(head); // 获得全局队列头的指针
	if (head_ptr == GP(q->tail)) { // 如果指针指向队列尾
		return NULL; // 返回全局队列为空
	}

	if(!q->flag[head_ptr]) { // 如果标志为假
		return NULL; // 返回全局队列为空
	}

	__sync_synchronize(); // 同步

	struct message_queue * mq = q->queue[head_ptr]; // 取出消息队列

	// 比较交换：如果&q->head == head，则将 head+1 写入 &q->head
	if (!__sync_bool_compare_and_swap(&q->head, head, head+1)) {
		return NULL;
	}
	q->flag[head_ptr] = false; // 设置标志为假

	return mq; // 返回消息队列的指针
}

/// 创建消息队列
/// \param[in] handle
/// \return struct message_queue *
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q)); // 分配内存
	q->handle = handle; // 句柄
	q->cap = DEFAULT_QUEUE_SIZE; // 默认队列大小
	q->head = 0; // 队列头
	q->tail = 0; // 队列尾
	q->lock = 0; // 队列锁
	q->in_global = MQ_IN_GLOBAL; // 在全局队列中
	q->release = 0; // 释放
	q->lock_session = 0; // 会话
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap); // 分配cap份内存

	return q; // 返回消息队列结构的指针
}

/// 释放
/// \param[in] *q
/// \return static void
static void 
_release(struct message_queue *q) {
	skynet_free(q->queue); // 释放消息队列的队列
	skynet_free(q); // 释放消息队列
}

/// 获得消息队列的句柄
/// \param[in] *q
/// \return uint32_t
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle; // 返回句柄的值
}

/// 获得消息队列的长度
/// \param[in] *q
/// \return int
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	LOCK(q) // 加锁
	head = q->head; // 队列头
	tail = q->tail; // 队列尾
	cap = q->cap;   // 默认队列大小
	UNLOCK(q) // 解锁
	
	if (head <= tail) { // 如果队列头小于等于队列尾
		return tail - head; // 返回队列尾 - 队列头
	}
	return tail + cap - head; // 否则返回队列尾 + 默认队列大小 - 队列头
}

/// 弹出消息队列
/// \param[in] *q
/// \param[in] *message
/// \return int
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1; // 失败
	LOCK(q) // 锁住

	if (q->head != q->tail) { // 如果队列头不等于队列尾
		*message = q->queue[q->head]; // 取出队列头
		ret = 0; // 弹出成功返回0
		if ( ++ q->head >= q->cap) { // 如果队列头 >= 最大队列数
			q->head = 0; // 队列头 head = 0
		}
	}

	if (ret) { // 弹出成功
		q->in_global = 0; // 设置在全局状态为0
	}
	
	UNLOCK(q) // 解锁

	return ret;
}

/// 展开队列
/// \param[in] *q
/// \return static void
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2); // 分配2倍cap内存
	int i;
	for (i=0;i<q->cap;i++) { // 循环所有
		new_queue[i] = q->queue[(q->head + i) % q->cap]; // 求余数，取出消息
	}
	q->head = 0; // 队列头
	q->tail = q->cap; // 队列尾
	q->cap *= 2; // 队列数
	
	skynet_free(q->queue); // 释放
	q->queue = new_queue; // 返回新的消息队列
}

/// 解锁
/// \param[in] *q
/// \return static void
static void
_unlock(struct message_queue *q) {
	// this api use in push a unlock message, so the in_global flags must not be 0 , 
	// but the q is not exist in global queue.
	if (q->in_global == MQ_LOCKED) { // 如果在全局队列标志为已锁
		skynet_globalmq_push(q); // 压入全局消息队列
		q->in_global = MQ_IN_GLOBAL; // 全局队列标志为在全局
	} else {
		assert(q->in_global == MQ_DISPATCHING); // 标志为调度
	}
	q->lock_session = 0; // 会话锁为0
}

/// 压入队列头
/// \param[in] *q
/// \param[in] *message
/// \return static void
static void 
_pushhead(struct message_queue *q, struct skynet_message *message) {
	int head = q->head - 1; // head为队列头 -1
	if (head < 0) { // 如果head小于0
		head = q->cap - 1; // 队列数 -1
	}
	if (head == q->tail) { // 如果 head 等于队列尾
		expand_queue(q); // 展开队列
		--q->tail; // 队列尾 -1
		head = q->cap - 1; // head 等于队列数 -1
	}

	q->queue[head] = *message; // 压入消息到队列
	q->head = head; // 队列头等于 head

	_unlock(q); // 解锁
}

/// 压入消息队列
/// \param[in] *q
/// \param[in] *message
/// \return void
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message); // 断言 消息是否存在
	LOCK(q) // 锁住消息队列
	
	// 如果会话锁不为0,且消息会话等于消息队列的会话锁
	if (q->lock_session !=0 && message->session == q->lock_session) {
		_pushhead(q,message); // 将消息压入消息队列的头
	} else {
		q->queue[q->tail] = *message; // 将消息压入消息队列的尾
		if (++ q->tail >= q->cap) { // 如果队列尾的值大于等于cap
			q->tail = 0; // 则，队列尾等于0
		}

		if (q->head == q->tail) { // 如果队列头等于队列尾
			expand_queue(q); // 展开队列
		}

		if (q->lock_session == 0) { // 如果会话标志等于0
			if (q->in_global == 0) { // 如果在全局标志等于0
				q->in_global = MQ_IN_GLOBAL; // 设置标志为在全局
				skynet_globalmq_push(q); // 压入全局消息队列
			}
		}
	}
	
	UNLOCK(q) // 解锁
}

/// 锁住消息队列
/// \param[in] *q
/// \param[in] session
/// \return void
void
skynet_mq_lock(struct message_queue *q, int session) {
	LOCK(q) // 加锁
	assert(q->lock_session == 0); //断言
	assert(q->in_global == MQ_IN_GLOBAL); // 断言
	q->in_global = MQ_DISPATCHING; // 在全局标志为调度
	q->lock_session = session; // 会话锁等于会话值
	UNLOCK(q) // 解锁
}

/// 解锁消息队列
/// \param[in] *q
/// \return void
void
skynet_mq_unlock(struct message_queue *q) {
	LOCK(q) // 加锁
	_unlock(q); // 解锁
	UNLOCK(q) // 解锁
}

/// 消息队列初始化
/// \return void
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q)); // 分配内存
	memset(q,0,sizeof(*q)); // 清空结构
	q->queue = skynet_malloc(MAX_GLOBAL_MQ * sizeof(struct message_queue *)); // 分配 MAX_GLOBAL_MQ 份内存
	q->flag = skynet_malloc(MAX_GLOBAL_MQ * sizeof(bool)); // 分配 MAX_GLOBAL_MQ 份内存
	memset(q->flag, 0, sizeof(bool) * MAX_GLOBAL_MQ); // 清空结构
	Q=q;
}

/// 强行压入消息队列
/// \param[in] *queue
/// \return void
void 
skynet_mq_force_push(struct message_queue * queue) {
	assert(queue->in_global); // 断言
	skynet_globalmq_push(queue); // 压入全局消息队列
}

/// 压入全局消息队列
/// \param[in] *queue
/// \return void
void 
skynet_mq_pushglobal(struct message_queue *queue) {
	LOCK(queue) // 加锁
	assert(queue->in_global); // 断言
	if (queue->in_global == MQ_DISPATCHING) { // 是否为调度
		// lock message queue just now.
		queue->in_global = MQ_LOCKED; // 锁住
	}
	if (queue->lock_session == 0) { // 会话锁是否为0
		skynet_globalmq_push(queue); // 压入全局消息队列
		queue->in_global = MQ_IN_GLOBAL; // 设置为全局
	}
	UNLOCK(queue) // 解锁
}

/// 标志释放消息队列
/// \param[in] *q
/// \return void
void 
skynet_mq_mark_release(struct message_queue *q) {
	LOCK(q) // 锁住
	assert(q->release == 0); // 断言
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) { // 标志不为在全局
		skynet_globalmq_push(q); // 压缩全局消息队列
	}
	UNLOCK(q) // 解锁
}

///
/// \param[in] *q
/// \return static int
static int
_drop_queue(struct message_queue *q) {
	// todo: send message back to message source
	struct skynet_message msg;
	int s = 0;
	while(!skynet_mq_pop(q, &msg)) {
		++s;
		skynet_free(msg.data);
	}
	_release(q);
	return s;
}

/// 释放消息队列
/// \param[in] *q
/// \return int
int 
skynet_mq_release(struct message_queue *q) {
	int ret = 0;
	LOCK(q) // 加锁
	
	if (q->release) {
		UNLOCK(q) // 解锁
		ret = _drop_queue(q);
	} else {
		skynet_mq_force_push(q); // 强行压入消息队列
		UNLOCK(q) // 解锁
	}
	
	return ret;
}
