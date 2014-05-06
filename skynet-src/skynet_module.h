#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

// 函数指针类型
typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

struct skynet_module {
	const char * name; // 模块名称
	void * module; // 模块指针
	skynet_dl_create create; // 创建函数的指针
	skynet_dl_init init; // 创建初始化函数的指针
	skynet_dl_release release; // 创建释放函数的指针
};

void skynet_module_insert(struct skynet_module *mod); // 插入模块到数组中
struct skynet_module * skynet_module_query(const char * name); // 查询模块是否在数组
void * skynet_module_instance_create(struct skynet_module *); // 创建函数
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm); // 初始化函数
void skynet_module_instance_release(struct skynet_module *, void *inst); // 释放函数

void skynet_module_init(const char *path); // 初始化模块

#endif
