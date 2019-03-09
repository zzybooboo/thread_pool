/*
author: zhangwei
*/

#ifndef PTHRED_POOL_INCLUDE
#define PTHRED_POOL_INCLUDE

#include <stdbool.h>

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC 
#endif  

typedef void(*task_handler_t)(void *);
typedef struct thread_pool_ * thread_pool_t;

typedef struct thread_pool_info_
{
	unsigned short        max_thread_count;		//最大线程数量
	unsigned short        min_thread_count;		//最低线程数量
	unsigned short		  current_thread_count;	//当前线程数量
	unsigned short        idle_thread_count;    //空闲线程数量
	size_t                waiting_tasks_count;  //等待执行任务数量
	size_t                idle_tasks_count;		//空闲任务数量
}*thread_pool_info_t;

//创建线程池
EXTERNC thread_pool_t      thread_pool_create(unsigned short min_thread_count, unsigned short max_thread_count);

//回收资源
EXTERNC void               thread_pool_shrink(thread_pool_t pool);

//创建待执行任务
EXTERNC bool               thread_pool_make_task(thread_pool_t pool, void * param, task_handler_t handler);

//销毁线程池
EXTERNC void		       thread_pool_destory(thread_pool_t pool);

//打印线程池状态报告
EXTERNC thread_pool_info_t thread_pool_info(thread_pool_t pool);
#endif
