#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	uint32_t source;        // 来源
	int session;            // 会话
	void * data;            // 数据的指针
	size_t sz;              // 数据的长度
};

struct message_queue;

struct message_queue * skynet_globalmq_pop(void); // 弹出全局消息队列

struct message_queue * skynet_mq_create(uint32_t handle); // 创建消息队列
void skynet_mq_mark_release(struct message_queue *q); // 标记释放消息队列
int skynet_mq_release(struct message_queue *q); // 释放消息队列
uint32_t skynet_mq_handle(struct message_queue *); // 消息队列的句柄

// 0 for success
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message); // 弹出消息队列
void skynet_mq_push(struct message_queue *q, struct skynet_message *message); // 压入消息队列
void skynet_mq_lock(struct message_queue *q, int session); // 锁住消息队列
void skynet_mq_unlock(struct message_queue *q); // 解锁消息队列

// return the length of message queue, for debug
int skynet_mq_length(struct message_queue *q); // 消息队列的长度

void skynet_mq_force_push(struct message_queue *q); // 强行弹出消息队列
void skynet_mq_pushglobal(struct message_queue *q); // 压入全局消息队列

void skynet_mq_init(); // 初始化消息队列

#endif
