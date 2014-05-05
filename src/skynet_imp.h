#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

// Skynet配置的结构
struct skynet_config {
	int thread; // 线程数
	int harbor; // 节点编号
	const char * logger; // 日志文件
	const char * module_path; // 模块的路径
	const char * master; // 节点连接master的地址
	const char * local; // 节点的地址
	const char * start; // 启动的 LUA服务
	const char * standalone; // master监听的地址
};

void skynet_start(struct skynet_config * config); // 启动 Skynet

#endif
