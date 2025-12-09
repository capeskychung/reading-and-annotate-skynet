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

// 忽略（屏蔽）SIGPIPE信号，当进程向一个已经关闭的管道或套接字socket写入数据时，操作系统会向进程发送SIGPIPE信号。
// 默认情况下，进程收到SIGPIPE后会直接中止，可能导致程序意外推出。
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN; // 指定信号的处理函数，SIG_IGN 是一个特殊值，表示 “忽略该信号”（Ignore）
	sa.sa_flags = 0; // 指定信号的处理函数，SIG_IGN 是一个特殊值，表示 “忽略该信号”（Ignore）
	sigemptyset(&sa.sa_mask); // sa_mask 是一个信号集，指定在处理当前信号时需要阻塞的其他信号。sigemptyset 函数将其初始化为空集，即处理 SIGPIPE 时不阻塞任何其他信号
	sigaction(SIGPIPE, &sa, 0); // 调用 sigaction 系统调用，将 SIGPIPE 信号的处理方式设置为 sa 结构体中定义的配置（忽略）。第三个参数为 0，表示不需要保存旧的信号处理配置
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
	// 命令行处理
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1]; // config file路径
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	// 全局初始化
	skynet_globalinit(); // 全局变量初始化 线程本地存储键和主线程标识 skynet_server.c globalinit
	skynet_env_init(); // 环境变量结构体初始化

	sigign(); // // 忽略 SIGPIPE 信号（避免网络操作时进程意外终止）

	struct skynet_config config;

// lua代码缓存初始化
#ifdef LUA_CACHELIB
	// init the lock of code cache
	// 初始化 Lua 代码缓存的锁（多线程安全相关）
	luaL_initcodecache();
#endif

	// 加载并执行配置文件
	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);	// link lua lib

	// 加载配置解析逻辑（字符串形式的 Lua 代码 load_config） 
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK); // 确保加载成功
	lua_pushstring(L, config_file); // 向 Lua 栈压入配置文件路径作为参数

	// 执行加载的配置解析代码（传入 1 个参数，期望 1 个返回值）
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1)); // 打印执行错误
		lua_close(L);
		return 1;
	}
	// 将lua配置表转换为skynet 环境变量
	_init_env(L);
	lua_close(L); // 关闭 Lua 虚拟机，配置加载完成

	// 5. 构建skynet配置结构体
	// 环境变量中读取配置（或使用默认值），构建 skynet_config 结构体，该结构体是启动 Skynet 的核心参数
	config.thread =  optint("thread",8);  // 工作线程数（默认 8）
	config.module_path = optstring("cpath","./cservice/?.so");  // C 服务模块路径（默认 ./cservice/?.so）
	config.harbor = optint("harbor", 1);  // 节点编号（默认 1，用于分布式部署）
	config.bootstrap = optstring("bootstrap","snlua bootstrap"); // 启动入口服务（默认 snlua bootstrap）
	config.daemon = optstring("daemon", NULL); // 守护进程日志文件路径（默认不启用） 
	config.logger = optstring("logger", NULL);  // 日志输出文件路径（默认控制台）
	config.logservice = optstring("logservice", "logger"); // 日志服务类型（默认 logger）
	config.profile = optboolean("profile", 1); // 是否启用性能分析（默认启用）

	// 启动 Skynet 框架核心服务
	skynet_start(&config); // skynet_start 是框架启动的核心函数，根据 config 参数初始化工作线程、启动入口服务（如 bootstrap），进入事件循环
	skynet_globalexit(); // 框架运行结束后，skynet_globalexit 执行全局资源清理， 全局的数据删除

	return 0;
}
// 校验参数 → 初始化环境 → 加载配置 → 构建启动参数 → 启动框架
