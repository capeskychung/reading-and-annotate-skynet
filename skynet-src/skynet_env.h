#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

const char * skynet_getenv(const char *key); // 从环境变量中获取某个key
void skynet_setenv(const char *key, const char *value); // 设置环境变量中的某key的值

void skynet_env_init(); // 环境变量初始化函数

#endif
