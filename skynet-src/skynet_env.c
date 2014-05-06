///
/// \file skynet_env.c
/// \brief LUA 环境设置
///
#include "skynet.h"
#include "skynet_env.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

struct skynet_env {
	int lock; ///< 锁
	lua_State *L; ///< LUA状态机
};

static struct skynet_env *E = NULL;

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {} ///< 加锁
#define UNLOCK(q) __sync_lock_release(&(q)->lock); ///< 解锁

/// 获得环境变量
/// \param[in] *key
/// \return const char *
const char * 
skynet_getenv(const char *key) {
	LOCK(E)

	lua_State *L = E->L;
	
	lua_getglobal(L, key);
	const char * result = lua_tostring(L, -1);
	lua_pop(L, 1);

	UNLOCK(E)

	return result;
}

/// 设置环境变量
/// \param[in] *key 键
/// \param[in] *value 值
/// \return void
void 
skynet_setenv(const char *key, const char *value) {
	LOCK(E)
	
	lua_State *L = E->L;
	lua_getglobal(L, key);
	assert(lua_isnil(L, -1));
	lua_pop(L,1);
	lua_pushstring(L,value);
	lua_setglobal(L,key);

	UNLOCK(E)
}

/// 初始化
/// \return void
void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	E->lock = 0;
	E->L = luaL_newstate();
}
