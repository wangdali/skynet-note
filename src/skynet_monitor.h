#ifndef SKYNET_MONITOR_H
#define SKYNET_MONITOR_H

#include <stdint.h>

struct skynet_monitor; // 监视的数据结构

struct skynet_monitor * skynet_monitor_new(); // 新建监视
void skynet_monitor_delete(struct skynet_monitor *); // 删除监视
void skynet_monitor_trigger(struct skynet_monitor *, uint32_t source, uint32_t destination); // 触发监视
void skynet_monitor_check(struct skynet_monitor *); // 检查监视

#endif
