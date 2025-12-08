# 引入平台相关的编译配置（比如不同系统的编译参数）
include platform.mk

# 定义默认输出路径
LUA_CLIB_PATH ?= luaclib  # Lua 扩展库输出目录
CSERVICE_PATH ?= cservice # C 服务模块输出目录

SKYNET_BUILD_PATH ?= .   # Skynet 主程序输出目录

# 编译选项：调试信息 + 优化 + 警告 + Lua 头文件路径
CFLAGS = -g -O2 -Wall -I$(LUA_INC) $(MYCFLAGS)
# CFLAGS += -DUSE_PTHREAD_LOCK # 可选：启用 pthread 锁（多线程场景）

# lua

# -------------------------- Lua 依赖 --------------------------
LUA_STATICLIB := 3rd/lua/liblua.a # Lua 静态库路径
LUA_LIB ?= $(LUA_STATICLIB) # 默认使用Lua静态库
LUA_INC ?= 3rd/lua  # Lua头文件路径


# 编译Lua静态库的规则：进入 3rd/lua 目录，按平台编译
$(LUA_STATICLIB) :
	cd 3rd/lua && $(MAKE) CC='$(CC) -std=gnu99' $(PLAT)

# https : turn on TLS_MODULE to add https support

# -------------------------- TLS/HTTPS 可选依赖 --------------------------
# TLS_MODULE=ltls # 启用时会编译 ltls.so，支持 HTTPS
TLS_LIB=  # TLS 库路径（如 OpenSSL）
TLS_INC=  # TLS 头文件路径

# jemalloc
# -------------------------- Jemalloc 内存分配器 --------------------------
JEMALLOC_STATICLIB := 3rd/jemalloc/lib/libjemalloc_pic.a # jemalloc 静态库
JEMALLOC_INC := 3rd/jemalloc/include/jemalloc # jemalloc 头文件

all : jemalloc 

.PHONY : jemalloc update3rd

MALLOC_STATICLIB := $(JEMALLOC_STATICLIB) # 默认使用 jemalloc

# 编译 jemalloc 的依赖链：先初始化子模块 → 生成 Makefile → 编译
$(JEMALLOC_STATICLIB) : 3rd/jemalloc/Makefile
	cd 3rd/jemalloc && $(MAKE) CC=$(CC)


3rd/jemalloc/autogen.sh :
	git submodule update --init # 拉取 jemalloc 源码（如果未初始化）

3rd/jemalloc/Makefile : | 3rd/jemalloc/autogen.sh
	cd 3rd/jemalloc && ./autogen.sh --with-jemalloc-prefix=je_ --enable-prof

jemalloc : $(MALLOC_STATICLIB) # 编译 jemalloc

update3rd : # 重置 jemalloc 子模块
	rm -rf 3rd/jemalloc && git submodule update --init 

# skynet

# 要编译的 C 服务模块（cservice）
CSERVICE = snlua logger gate harbor

# 要编译的 Lua 扩展库（luaclib）
LUA_CLIB = skynet \
  client \
  bson md5 sproto lpeg $(TLS_MODULE)

LUA_CLIB_SKYNET = \
  lua-skynet.c lua-seri.c \
  lua-socket.c \
  lua-mongo.c \
  lua-netpack.c \
  lua-memory.c \
  lua-multicast.c \
  lua-cluster.c \
  lua-crypt.c lsha1.c \
  lua-sharedata.c \
  lua-stm.c \
  lua-debugchannel.c \
  lua-datasheet.c \
  lua-sharetable.c \
  \

# Skynet 主程序的源码文件列表
SKYNET_SRC = skynet_main.c skynet_handle.c skynet_module.c skynet_mq.c \
  skynet_server.c skynet_start.c skynet_timer.c skynet_error.c \
  skynet_harbor.c skynet_env.c skynet_monitor.c skynet_socket.c socket_server.c \
  mem_info.c malloc_hook.c skynet_daemon.c skynet_log.c

# `make all` 的核心目标：编译主程序 + 所有 C 服务 + 所有 Lua 扩展
all : \
  $(SKYNET_BUILD_PATH)/skynet \
  $(foreach v, $(CSERVICE), $(CSERVICE_PATH)/$(v).so) \
  $(foreach v, $(LUA_CLIB), $(LUA_CLIB_PATH)/$(v).so)


# 用指定的编译器（CC）和编译选项（CFLAGS），将所有源码和静态库链接成可执行文件 skynet
$(SKYNET_BUILD_PATH)/skynet : $(foreach v, $(SKYNET_SRC), skynet-src/$(v)) $(LUA_LIB) $(MALLOC_STATICLIB)
	$(CC) $(CFLAGS) -o $@ $^ -Iskynet-src -I$(JEMALLOC_INC) $(LDFLAGS) $(EXPORT) $(SKYNET_LIBS) $(SKYNET_DEFINES)

# 创建输出目录（如果不存在）
$(LUA_CLIB_PATH) :
	mkdir $(LUA_CLIB_PATH)

$(CSERVICE_PATH) :
	mkdir $(CSERVICE_PATH)

# 定义 C 服务模块的编译模板（批量生成规则）
define CSERVICE_TEMP
  $$(CSERVICE_PATH)/$(1).so : service-src/service_$(1).c | $$(CSERVICE_PATH)
	$$(CC) $$(CFLAGS) $$(SHARED) $$< -o $$@ -Iskynet-src
endef

# 为每个 CSERVICE 模块生成编译规则（如 snlua → service_snlua.c → snlua.so）
$(foreach v, $(CSERVICE), $(eval $(call CSERVICE_TEMP,$(v))))

# 编译核心 skynet.so（Skynet Lua API 核心）
$(LUA_CLIB_PATH)/skynet.so : $(addprefix lualib-src/,$(LUA_CLIB_SKYNET)) | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Iskynet-src -Iservice-src -Ilualib-src

# 编译 bson.so（BSON 序列化）
$(LUA_CLIB_PATH)/bson.so : lualib-src/lua-bson.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -Iskynet-src $^ -o $@
# 编译md5.so MD5 加密
$(LUA_CLIB_PATH)/md5.so : 3rd/lua-md5/md5.c 3rd/lua-md5/md5lib.c 3rd/lua-md5/compat-5.2.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -I3rd/lua-md5 $^ -o $@

# 编译client.so 客户端网络
$(LUA_CLIB_PATH)/client.so : lualib-src/lua-clientsocket.c lualib-src/lua-crypt.c lualib-src/lsha1.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -lpthread

# 编译 sproto.so 协议序列化
$(LUA_CLIB_PATH)/sproto.so : lualib-src/sproto/sproto.c lualib-src/sproto/lsproto.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -Ilualib-src/sproto $^ -o $@

# 编译 ltls.so（TLS/HTTPS，需启用 TLS_MODULE）
$(LUA_CLIB_PATH)/ltls.so : lualib-src/ltls.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -Iskynet-src -L$(TLS_LIB) -I$(TLS_INC) $^ -o $@ -lssl

# 编译 lpeg.so（Lua 模式匹配库）
$(LUA_CLIB_PATH)/lpeg.so : 3rd/lpeg/lpcap.c 3rd/lpeg/lpcode.c 3rd/lpeg/lpprint.c 3rd/lpeg/lptree.c 3rd/lpeg/lpvm.c 3rd/lpeg/lpcset.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -I3rd/lpeg $^ -o $@

# 清理编译产物（主程序 + 动态库 + 调试符号）
clean :
	rm -f $(SKYNET_BUILD_PATH)/skynet $(CSERVICE_PATH)/*.so $(LUA_CLIB_PATH)/*.so && \
  rm -rf $(SKYNET_BUILD_PATH)/*.dSYM $(CSERVICE_PATH)/*.dSYM $(LUA_CLIB_PATH)/*.dSYM
	$(MAKE) clean -f mingw.mk

# 深度清理：包括 jemalloc 和 Lua 的编译产物
cleanall: clean
ifneq (,$(wildcard 3rd/jemalloc/Makefile))
	cd 3rd/jemalloc && $(MAKE) clean && rm Makefile
endif
	cd 3rd/lua && $(MAKE) clean
	rm -f $(LUA_STATICLIB)
	$(MAKE) cleanall -f mingw.mk
