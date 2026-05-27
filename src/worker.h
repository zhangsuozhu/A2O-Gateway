/**
 * @file worker.h
 * @brief 工作线程池公共接口头文件
 *
 * 本文件定义了网关工作线程池的对外接口。
 * 工作线程池负责将客户端请求封装为 gateway_job_t 任务，
 * 并通过 libcurl multi 接口并发转发到上游 LLM 服务提供商。
 * 线程池采用轮询（round-robin）方式将任务分发给各个 worker，
 * 每个 worker 独立运行一个 libcurl multi 事件循环。
 */

#ifndef WORKER_H
#define WORKER_H

#include "types.h"

/**
 * @brief 将任务入队到工作线程池
 * @param job 指向已分配并填充好的 gateway_job_t 任务
 *
 * 采用轮询策略（RR++ % WORKER_COUNT）选择目标 worker，
 * 将任务追加到该 worker 的 pending 链表尾部，并唤醒 worker 线程。
 * 该函数线程安全，可由 libevent 主线程在收到 HTTP 请求后调用。
 */
void enqueue_job(gateway_job_t *job);

/**
 * @brief 启动所有工作线程
 *
 * 根据全局配置 WORKER_COUNT 创建若干 worker 线程。
 * 每个 worker 初始化独立的 pthread_mutex_t、pthread_cond_t，
 * 然后启动 worker_loop 线程，进入 libcurl multi 事件循环。
 * 本函数应在 event_base_dispatch 之前调用。
 */
void workers_start(void);

/**
 * @brief 优雅停止所有工作线程
 *
 * 向每个 worker 设置 stop 标志并发送条件变量信号，
 * 随后 pthread_join 等待所有线程退出。
 * 调用后所有 worker 的 multi handle 会被清理。
 */
void workers_stop(void);

/**
 * @brief 释放 gateway_job_t 占用的全部资源
 * @param job 任务指针（允许为 NULL，空操作）
 *
 * 释放字符串字段、内存缓冲区、stream 状态、curl handle/headers、
 * 客户端请求（若尚未发送响应）、send buffer、互斥锁等。
 * 如果 job->client_req 仍存活且 BASE 存在，会通过 event_base_once
 * 延迟到 libevent 主线程执行 evhttp_request_free，避免跨线程释放。
 */
void job_free(gateway_job_t *job);

#endif