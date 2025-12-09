#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

// 从 Skynet 框架的全局环境中获取整数类型的配置项，若该配置项不存在则使用默认值并将其存入环境中
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key); // 从全局环境中查询 key 对应的字符串值
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt); // 将默认值 opt 转换为字符串
		skynet_setenv(key, tmp);  //  // 将键值对 (key, 默认值字符串) 存入环境
		return opt;
	}
	return strtol(str, NULL, 10); // strtol 函数将字符串 str 转换为十进制整数并返回（strtol 第三个参数 10 表示按十进制解析）
}

// 从 Skynet 框架的全局环境中获取布尔类型的配置项，若该配置项不存在则使用默认值并将其存入环境中
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

/*
function lua_next(L, table_idx):
    弹出栈顶的 key
    从 table 中找 key 的下一个键值对 (next_key, next_val)
    if 找到:
        将 next_key 压入栈
        将 next_val 压入栈
        return 1
    else:
        return 0
*/


//  Lua 状态机中的配置表（table）内容导入到 Skynet 全局环境中（通过 skynet_setenv 存储）
static void
_init_env(lua_State *L) {
	lua_pushnil(L);  /* first key */ // 向 Lua 栈压入 nil，作为遍历表的初始键（配合 lua_next 使用）。此时栈结构（从底到顶）为：[配置表, nil] lua_next根据栈顶的值遍历key， nil表示lua_next没有遍历过
	while (lua_next(L, -2) != 0) { // lua_next(L, -2)：从栈中索引 -2（即配置表）中获取下一组键值对，将键压入栈，再将值压入栈。若遍历结束（无更多键值对），返回 0 退出循环 
		// 每次循环开始lua_next执行后，栈结构为：[配置表, 当前键, 当前值]（循环开始时通过 lua_next 填充）
		int keyt = lua_type(L, -2); // // 获取当前键的类型（栈索引 `-2` 为键）
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n"); // 键必须是字符串类型，否则报错退出
			exit(1);
		}
		const char * key = lua_tostring(L,-2);  // 将键转换为 C 字符串
		if (lua_type(L,-1) == LUA_TBOOLEAN) { // 如果值是bool类型
			int b = lua_toboolean(L,-1); // 将值转为bool类型
			skynet_setenv(key,b ? "true" : "false" ); // 将key value设置进env中
		} else {
			const char * value = lua_tostring(L,-1); // 栈索引 `-1` 为值
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);  // 直接存储字符串值
		}
		lua_pop(L,1);  // 弹出当前值，保留键用于下一次 `lua_next` 遍历
		// 每次处理完一组键值对后，弹出栈顶的 “值”，使栈恢复为 [配置表, 当前键]，这个当前键便是下一次next的上一个键，以便 lua_next 继续获取下一组数据。
	}
	lua_pop(L,1); // 历结束后，栈中剩余的是 [配置表, 最后一个键]，彻底清理栈中与配置表相关的内容
}

int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	skynet_globalinit();
	skynet_env_init();

	sigign();

	struct skynet_config config;

#ifdef LUA_CACHELIB
	// init the lock of code cache
	luaL_initcodecache();
#endif

	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);	// link lua lib

	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	lua_pushstring(L, config_file);

	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	_init_env(L);
	lua_close(L);

	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);
	config.logservice = optstring("logservice", "logger");
	config.profile = optboolean("profile", 1);

	skynet_start(&config);
	skynet_globalexit();

	return 0;
}
