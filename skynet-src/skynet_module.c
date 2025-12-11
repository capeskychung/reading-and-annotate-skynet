#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

// 管理 Skynet 框架中的模块（动态加载的组件）
struct modules {
	int count; // 当前已加载的模块数量
	struct spinlock lock;
	const char * path;  // 存储模块的搜索路径字符串，该路径用于指定动态链接库（如 .so 文件）的加载位置，支持通过分隔符（如 ;）指定多个路径
	struct skynet_module m[MAX_MODULE_TYPE]; // 存储已加载模块的数组，每个元素是一个 skynet_module 结构体，用于保存模块的名称和句柄
};

static struct modules * M = NULL;

// 根据指定的模块名称和搜索路径，尝试动态加载对应的模块（动态链接库）
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;  // 获取模块搜索路径（可能包含多个路径，用 ';' 分隔）
	size_t path_size = strlen(path); // 搜索路径总长度
	size_t name_size = strlen(name);  // 模块名称长度
 
	int sz = path_size + name_size; // 计算临时路径缓冲区大小（路径+模块名的最大可能长度）
	//search path
	void * dl = NULL;
	char tmp[sz]; // 临时缓冲区，用于拼接实际要尝试的路径
	do
	{
		memset(tmp,0,sz); // 内存初始化
		while (*path == ';') path++; // 跳过路径最前面的分隔符 ';'
		if (*path == '\0') break; // 如果路径为空，则退出循环
		l = strchr(path, ';'); // 查找路径中第一个的分隔符 ';'
		if (l == NULL) l = path + strlen(path); // 如果没有找到分隔符，则将 l 指向路径末尾
		int len = l - path; // 计算路径部分的长度
		int i;

		// 拼接实际加载路径： 搜索路径中用 ? 作为模块名称的占位符
		// 路径可能是 ./cservice/?.so;./cservice/?.dylib
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size); // 将占位符换成模块name
		if (path[i] == '?') {
			// 将占位符后的内容copy到临时路径缓冲区的末尾 复制 ? 之后的路径部分（如文件后缀 .so）
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			//  若路径中没有 ? 占位符，视为无效路径，直接退出程序
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		// dlopen 函数加载拼接好的路径 tmp
		// 标志 RTLD_NOW：加载时立即解析所有符号（不延迟）。
		// 标志 RTLD_GLOBAL：加载的符号可被其他动态库使用
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

// 接收模块名称（const char * name），返回指向 skynet_module 结构体的指针（找到则返回对应模块，否则返回 NULL）
static struct skynet_module *
_query(const char * name) {
	int i;
	// 遍历已加载的模块列表（M->m 数组，长度为 M->count）
	for (i=0;i<M->count;i++) {
		// 对比当前遍历到的模块名称（M->m[i].name）与目标名称（name）
		if (strcmp(M->m[i].name,name)==0) {
			// 若名称匹配，返回该模块的指针
			return &M->m[i];
		}
	}
	return NULL;
}


// 从已加载的模块（动态链接库）中查找并返回指定名称的 API 函数指针
static void *
get_api(struct skynet_module *mod, const char *api_name) {
	// 计算模块名和API后缀的长度
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	// 分配临时缓冲区（+1 用于存储字符串结束符 '\0'）
	char tmp[name_size + api_size + 1];
	// 拼接模块名到 tmp 头部
	memcpy(tmp, mod->name, name_size);
	// 拼接 API 后缀（包含其结束符）
	memcpy(tmp+name_size, api_name, api_size+1);
	// 使用 strrchr(tmp, '.') 查找字符串中最后一个点号（.）
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	// 通过 dlsym 函数从模块的动态链接库句柄（mod->module）中查找上一步得到的符号名，返回对应的函数指针
	return dlsym(mod->module, ptr);
}

static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create"); // 绑定模块实例创建函数（如 xxx_create）
	mod->init = get_api(mod, "_init"); // 绑定模块实例初始化函数（如 xxx_init）
	mod->release = get_api(mod, "_release"); // 绑定模块实例释放函数
	mod->signal = get_api(mod, "_signal"); // 绑定模块实例信号处理函数

	return mod->init == NULL; //  init 函数不存在（mod->init 为 NULL），返回 1（表示模块接口不完整，加载失败）
}

// 查询模块，模块存在时直接返回，不存在时，重新打开模块，并返回
struct skynet_module *
skynet_module_query(const char * name) {
	// 从全局模块管理器中查询模块
	struct skynet_module * result = _query(name);
	if (result) // 存在返回
		return result;

	SPIN_LOCK(M)

	result = _query(name); // double check

	if (result == NULL && M->count < MAX_MODULE_TYPE) { // mokuai
		int index = M->count;
		void * dl = _try_open(M,name); // 打开模块
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

			// 打开模块成功，进一步检查模块接口是否完整
			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	SPIN_UNLOCK(M)

	return result;
}

// 调用mod的create函数
void *
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();  // 若模块定义了 create 函数，则调用它创建实例并返回
	} else {
		return (void *)(intptr_t)(~0);  // 若没有 create 函数，返回特殊值
	}
}

// 调用mod的init函数
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

// 调用mod的release函数
void
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

// 调用mod的signal函数
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

// 初始化模块管理器,全局只调用一次，用于设置模块搜索路径并初始化相关结构
void
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path); // module的父级目录

	SPIN_INIT(m)

	M = m;
}
