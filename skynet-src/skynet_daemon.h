#ifndef skynet_daemon_h
#define skynet_daemon_h

int daemon_init(const char *pidfile);
int daemon_exit(const char *pidfile);

#endif

// daemon() 是 Linux 下专门用于将普通进程转为守护进程（Daemon Process） 的系统函数，
// 守护进程是脱离终端、在后台持续运行的特殊进程
// （如服务进程 sshd、nginx），通常用于执行周期性任务或提供长期服务

// int daemon(int nochdir, int noclose);
/*
nochdir：
* 0 ： 将守护进程的工作目录切换到/，避免当前目录被卸载导致进程异常
* 非0: 不修改工作目录

noclose:
* 0 : 将标准输入/输出/错误 重定向到/dev/null
* 非0: 不关闭/不重定向标准文件描述符
*/ 

/*
守护进程的核心特性
脱离终端：无控制终端（TTY），不会被终端的信号（如 Ctrl+C）打断；
后台运行：进程组组长、会话首进程，独立于终端会话；
忽略挂起信号：忽略 SIGHUP（终端关闭时发送的挂起信号）；
工作目录稳定：默认切换到 /，避免依赖临时目录；
标准 IO 重定向：默认指向 /dev/null（避免占用终端输出）。
*/


/*
daemon() 本质是封装了守护进程的创建流程，核心步骤如下：
第一次 fork ()：创建子进程，父进程退出（脱离原终端会话，子进程变为孤儿进程，由 init/systemd 接管）；
设置新会话（setsid ()）：子进程成为新会话的首进程、新进程组的组长，彻底脱离原终端；
第二次 fork ()（部分实现）：再次创建子进程，父进程退出（避免守护进程意外获取终端）；
切换工作目录：若 nochdir=0，切换到 /；
重定向标准 IO：若 noclose=0，关闭 STDIN/STDOUT/STDERR，并重定向到 /dev/null；
重置文件权限掩码（umask）：通常设为 0，避免继承父进程的权限限制（部分实现）。
*/