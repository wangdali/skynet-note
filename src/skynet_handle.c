#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4

struct handle_name {
	char * name; // 名字
	uint32_t handle; // 句柄
};

struct handle_storage {
	struct rwlock lock; // 锁

	uint32_t harbor; // 节点
	uint32_t handle_index; // 句柄引索
	int slot_size; // 槽的大小
	struct skynet_context ** slot;
	
	int name_cap;
	int name_count;
	struct handle_name *name; // 句柄的名字
};

static struct handle_storage *H = NULL; // 全局结构变量的指针

// 注册句柄
uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H; // 设置为全局变量

	rwlock_wlock(&s->lock); // 加锁
	
	for (;;) {
		int i;
		for (i=0;i<s->slot_size;i++) { // 循环槽的大小次
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK;
			int hash = handle & (s->slot_size-1);
			if (s->slot[hash] == NULL) {
				s->slot[hash] = ctx;
				s->handle_index = handle + 1;

				rwlock_wunlock(&s->lock); // 解锁

				handle |= s->harbor;
				skynet_context_init(ctx, handle); // 初始化 Context 结构
				return handle;
			}
		}
		assert((s->slot_size*2 - 1) <= HANDLE_MASK); // 断言
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *)); // 分配内存
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *)); // 清空结构
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL); // 断言
			new_slot[hash] = s->slot[i];
		}
		skynet_free(s->slot); // 释放
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}

// 收回句柄
void
skynet_handle_retire(uint32_t handle) {
	struct handle_storage *s = H; // 全局变量

	rwlock_wlock(&s->lock); // 加锁

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		skynet_context_release(ctx); // 释放 Context 结构
		s->slot[hash] = NULL;
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;
	}

	rwlock_wunlock(&s->lock); // 解锁
}

// 回收所有句柄
void 
skynet_handle_retireall() {
	struct handle_storage *s = H; // 全局变量
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock); // 加锁
			struct skynet_context * ctx = s->slot[i];
			rwlock_runlock(&s->lock); // 解锁
			if (ctx != NULL) {
				++n;
				skynet_handle_retire(skynet_context_handle(ctx)); // 回收句柄
			}
		}
		if (n==0)
			return;
	}
}

struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

// 根据名字查找句柄
uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H; // 全局变量

	rwlock_rlock(&s->lock); // 加锁

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock); // 解锁

	return handle;
}

// 在之前插入名字
static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
		s->name_cap *= 2;
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		int i;
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

// 插入名字
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	char * result = skynet_strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

// 初始化句柄
void 
skynet_handle_init(int harbor) {
	assert(H==NULL); // 断言
	struct handle_storage * s = skynet_malloc(sizeof(*H)); // 分配内存
	s->slot_size = DEFAULT_SLOT_SIZE; // 设置默认槽的大小 =4
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *)); // 分配slot_size份内存
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	rwlock_init(&s->lock); // 初始化锁
	// reserve 0 for system
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name)); // 分配内存

	H = s; // 设置全局变量

	// Don't need to free H ，不需要释放 H
}

