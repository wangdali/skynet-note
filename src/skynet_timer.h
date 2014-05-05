#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

int skynet_timeout(uint32_t handle, int time, int session); // 超时
void skynet_updatetime(void); // 更新时间
uint32_t skynet_gettime(void); // 获得时间
uint32_t skynet_gettime_fixsec(void); // 获得启动时间

void skynet_timer_init(void); // 初始化定时器

#endif
