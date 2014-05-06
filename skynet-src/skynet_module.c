///
/// \file skynet_module.c
/// \brief 加载动态链接库
///
#include "skynet.h"

#include "skynet_module.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32 ///< 最大模块的类型

/// 模块的结构
struct modules {
	int count; ///< 模块的总数
	int lock; ///< 锁
	const char * path; ///< 模块的路径
	struct skynet_module m[MAX_MODULE_TYPE]; ///< 模块的数组
};

static struct modules * M = NULL; ///< 模块结构的全局指针变量

/// 尝试打开模块
/// \param[in] *m 模块的结构
/// \param[in] *name 模块的名称
/// \return static void *
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path; // 从结构中获得模块的路径
	size_t path_size = strlen(path); // 路径的长度
	size_t name_size = strlen(name); // 模块名称的长度

	int sz = path_size + name_size; // 模块路径和名称的长度
	//search path
	void * dl = NULL; // 模块的指针变量
	char tmp[sz]; // 模块的路径和名称变量
	do
	{
		memset(tmp,0,sz); // 清空
		while (*path == ';') path++; // 循环是否等于 ';' 分号
		if (*path == '\0') break; // 如果等于 '\0' 则跳出循环
		l = strchr(path, ';'); // 返回字串中第一次出现 ';' 分号的位置
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i]; // 循环取得‘？’之前的字符，到临时字符串中。
		}
		memcpy(tmp+i,name,name_size); // 复制模块名字到临时字符串中。
		if (path[i] == '?') { // 如果下一个字符为 '?' 则：
			strncpy(tmp+i+name_size,path+i+1,len - i - 1); // 从后面复制模块的后缀名到临时字符串中。
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL); // 打开模块的动态链接库
		path = l;
	}while(dl == NULL); // 循环所有路径，以';'分号为界

	if (dl == NULL) { // 如果打开模块文件失败
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl; // 返回打开模块的指针变量
}

/// 根据名称查询模块数组，返回模块结构的指针
/// \param[in] *name 模块名称
/// \return static struct skynet_module *
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) { // 循环所有模块
		if (strcmp(M->m[i].name,name)==0) { // 比较模块名
			return &M->m[i]; // 返回模块结构的指针
		}
	}
	return NULL;
}

/// 打开函数
/// \param[in] *mod 模块的结构
/// \return static int
static int
_open_sym(struct skynet_module *mod) {
	size_t name_size = strlen(mod->name); // 模块名称的长度
	char tmp[name_size + 9]; // create/init/release , longest name is release (7)
	memcpy(tmp, mod->name, name_size); // 复制模块名称到临时字符串
	strcpy(tmp+name_size, "_create"); // 复制‘_create’到临时字符串
	mod->create = dlsym(mod->module, tmp); // 打开模块名+‘_create’的函数
	strcpy(tmp+name_size, "_init"); // 复制‘_init’到临时字符串
	mod->init = dlsym(mod->module, tmp); // 打开模块名+‘_init’的函数
	strcpy(tmp+name_size, "_release"); // 复制‘_release’到临时字符串
	mod->release = dlsym(mod->module, tmp); // 打开模块名+‘_release’的函数

	return mod->init == NULL; // 判断是否为空，返回真或假
}

/// 查询模块
/// \param[in] *name 模块的名称
/// \return struct skynet_module *
struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name); // 检查模块数组是否有这个模块
	if (result)
		return result; // 如果存在这个模块，则返回这个模块的指针

	while(__sync_lock_test_and_set(&M->lock,1)) {} // 尝试锁上模块数组

	result = _query(name); // double check 再次检查模块数组是否有这个模块

	if (result == NULL && M->count < MAX_MODULE_TYPE) { // 如果没有找到，且模块总数小于最大模块数
		int index = M->count; // 设置模块数组的下标为模块数
		void * dl = _try_open(M,name); // 尝试打开模块
		if (dl) { // 如果打开模块成功
			M->m[index].name = name; // 将名字赋值给模块数组中的元素
			M->m[index].module = dl; // 将模块指针赋值给模块数组中的元素

			if (_open_sym(&M->m[index]) == 0) { // 打开模块中的函数
				M->m[index].name = skynet_strdup(name);
				M->count ++; // 模块数 +1
				result = &M->m[index]; // 返回模块的结构指针
			}
		}
	}

	__sync_lock_release(&M->lock); // 解锁

	return result; // 返回模块的结构指针
}

/// 插入模块结构到模块数组中
/// \param[in] *mod 模块的结构
/// \return void
void 
skynet_module_insert(struct skynet_module *mod) {
	while(__sync_lock_test_and_set(&M->lock,1)) {} // 尝试锁住模块数组

	struct skynet_module * m = _query(mod->name); // 查询模块是否在数组中
	assert(m == NULL && M->count < MAX_MODULE_TYPE); // 断言模块为空，且总数不大于模块最大数
	int index = M->count; // 设置模块数组的下标为模块数
	M->m[index] = *mod; // 将模块结构指针赋值给数组
	++M->count; // 模块数 +1
	__sync_lock_release(&M->lock); // 解锁
}

/// 实例化创建函数
/// \param[in] *m
/// \return void *
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) { // 如果存在这个指针
		return m->create(); // 执行模块中的创建函数
	} else {
		return (void *)(intptr_t)(~0); // 返回空指针
	}
}

/// 实例化初始化函数
/// \param[in] *m
/// \param[in] *inst
/// \param[in] *ctx
/// \param[in] *parm
/// \return int
/// \retval 0 成功
/// \retval 1 失败
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm); // 执行模块中的初始化函数
}

/// 实例化释放函数
/// \param[in] *m
/// \param[in] *inst
/// \return void
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) { // 如果存在这个指针
		m->release(inst); // 执行模块中的释放函数
	}
}

/// 模块初始化
/// \param[in] *path
/// \return void
void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m)); // 分配模块结构的内存
	m->count = 0; // 模块总是为0
	m->path = skynet_strdup(path); // 模块的路径
	m->lock = 0; // 模块的锁为0

	M = m; // 赋值给全局模块结构变量
}
