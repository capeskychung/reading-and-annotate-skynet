#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

struct skynet_env {
	struct spinlock lock;
	lua_State *L; 
};

static struct skynet_env *E = NULL;
// 环境变量存储在一个全局的 Lua 状态机（E->L）中，以 Lua 全局变量的形式存在

// 线程安全地读取存储在 Lua 状态机中的全局变量
const char * 
skynet_getenv(const char *key) {
	// SPIN_LOCK(E) 和 SPIN_UNLOCK(E) 是自旋锁宏，用于保护临界区代码
	SPIN_LOCK(E)

	lua_State *L = E->L; // 获取全局的Lua状态机
	
	lua_getglobal(L, key); // 将Lua全局变量中的key对应的值入栈
	const char * result = lua_tostring(L, -1); // / 从栈顶（-1 位置）获取字符串值
	lua_pop(L, 1); // 弹出栈顶元素，清理 Lua 栈

	SPIN_UNLOCK(E)

	return result;
}

void 
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)
	
	lua_State *L = E->L; // 获取全局Lua状态机
	lua_getglobal(L, key); // 查找key对应的全局变量并压入堆栈
	assert(lua_isnil(L, -1)); // 断言 栈顶元素必须是nil 即key不存在
	lua_pop(L,1); // 弹出栈顶的nil，清理栈
	lua_pushstring(L,value); // 将value字符串压入栈
	lua_setglobal(L,key); // 将栈顶的元素复制给Lua全局变量 key

	SPIN_UNLOCK(E)
}

void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E)); // 分配全局静态的环境变量内存，
	SPIN_INIT(E) // 自旋锁初始化
	E->L = luaL_newstate(); // 初始化E之中的lua虚拟机
}
