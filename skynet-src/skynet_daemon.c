#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include "skynet_daemon.h"

static int
check_pid(const char *pidfile) {
	int pid = 0;
	// 尝试打开 PID 文件
	FILE *f = fopen(pidfile,"r");
	if (f == NULL)
	// 若文件不存在或无法打开（f == NULL），直接返回 0（表示无已运行进程的记录）
		return 0;

	// 读取文件中的 PID
	int n = fscanf(f,"%d", &pid);
	fclose(f);

	if (n !=1 || pid == 0 || pid == getpid()) {
		// n != 1，即未读到有效的整数
		// 0 是无效 PID
		// PID 与当前进程 ID（getpid()）相同
		return 0;
	}

	// 使用 kill(pid, 0) 系统调用检查该 PID 对应的进程是否存在
	// kill 函数的第二个参数为 0 时，不会发送任何信号，仅用于检测进程是否存在
	if (kill(pid, 0) && errno == ESRCH)
		return 0;

	return pid;
}

// 将当前进程的 PID（进程 ID）写入指定的 PID 文件，
// 并通过文件锁机制确保同一时间只有一个进程能操作该文件，避免进程重复启动
static int
write_pid(const char *pidfile) {
	//打开 / 创建 PID 文件
	FILE *f;
	int pid = 0;
	// 以 读写模式（O_RDWR） 和 不存在则创建（O_CREAT） 的方式打开 / 创建 PID 文件，权限设置为 0644（所有者可读写，组和其他用户只读）
	int fd = open(pidfile, O_RDWR|O_CREAT, 0644);
	if (fd == -1) {
		// 文件打开失败
		fprintf(stderr, "Can't create pidfile [%s].\n", pidfile);
		return 0;
	}

	// 2. 将文件描述符转换为文件流
	f = fdopen(fd, "w+");
	// 模式 "w+" 表示读写模式，若文件存在则截断，不存在则创建（与 open 的 O_CREAT 配合）
	if (f == NULL) {
		// 失败返回0
		fprintf(stderr, "Can't open pidfile [%s].\n", pidfile);
		return 0;
	}

	// 获取文件排他锁
	// LOCK_EX 排他锁 独占所
	// LOCK_NB 非阻塞模式
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		int n = fscanf(f, "%d", &pid);
		fclose(f);
		if (n != 1) {
			fprintf(stderr, "Can't lock and read pidfile.\n");
		} else {
			fprintf(stderr, "Can't lock pidfile, lock is held by pid %d.\n", pid);
		}
		return 0;
	}

	// 写入当前进程pid
	pid = getpid();
	if (!fprintf(f,"%d\n", pid)) {
		// 写入pid文件失败了
		fprintf(stderr, "Can't write pid.\n");
		close(fd);
		return 0;
	}
	// 强制刷新缓冲区，确保 PID 立即写入磁盘（避免缓存导致的信息丢失）
	fflush(f);

	return pid;
}

// 将进程的标准输入（stdin）、标准输出（stdout）和标准错误（stderr）重定向到 /dev/null，这是守护进程（daemon）编程中的常见操作
// 重定向多个文件描述符
static int
redirect_fds() {
	// 打开 /dev/null 设备文件, 以读写模式打开该文件，获取文件描述符 nfd
	int nfd = open("/dev/null", O_RDWR);
	if (nfd == -1) {
		perror("Unable to open /dev/null: ");
		return -1;
	}
	// 重定向标准输入（文件描述符 0）
	if (dup2(nfd, 0) < 0) {
		perror("Unable to dup2 stdin(0): ");
		return -1;
	}
	// 重定向标准输出（文件描述符 1）
	if (dup2(nfd, 1) < 0) {
		perror("Unable to dup2 stdout(1): ");
		return -1;
	}
	// 重定向标准错误（文件描述符 2）
	if (dup2(nfd, 2) < 0) {
		perror("Unable to dup2 stderr(2): ");
		return -1;
	}
	// 关闭临时文件描述符
	close(nfd);

	return 0;
}


// 从skynet_start.c 中的skynet_start函数开启
// 将当前进程转换为守护进程（daemon），并通过 PID 文件管理进程状态，避免重复启动，
// 同时重定向标准输入 / 输出 / 错误流，确保后台运行不依赖终端
int
daemon_init(const char *pidfile) {
	// 检查进程是否已运行
	int pid = check_pid(pidfile);

	// 非0 表示进程已经存在
	if (pid) {
		fprintf(stderr, "Skynet is already running, pid = %d.\n", pid);
		return 1;
	}

#ifdef __APPLE__
// 由于 daemon 函数在 macOS 10.5 后被弃用，仅打印警告信息（不执行 daemon 调用，可能依赖其他方式后台运行）
	fprintf(stderr, "'daemon' is deprecated: first deprecated in OS X 10.5 , use launchd instead.\n");
#else
	// 成为守护进程
	// 进程 daemon 化（跨平台处理）
	if (daemon(1,1)) {
		fprintf(stderr, "Can't daemonize.\n");
		return 1;
	}
#endif
	// 写入 PID 文件
	pid = write_pid(pidfile);
	if (pid == 0) {
		return 1;
	}
	// 重定向标准文件描述符
	if (redirect_fds()) {
		return 1;
	}

	return 0;
}

int
daemon_exit(const char *pidfile) {
	// 移除文件系统中的目录项（硬链接），并在文件的链接计数减至 0 且无进程打开该文件时，彻底删除文件数据。它是文件删除的底层实现（rm 命令本质就是调用 unlink()）。

	return unlink(pidfile);
}
