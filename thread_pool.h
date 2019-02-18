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

//创建线程池
EXTERNC thread_pool_t  thread_pool_create(unsigned short min_count, unsigned short max_count);

//回收资源
EXTERNC void           thread_pool_shrink(thread_pool_t pool);

//创建待执行任务
EXTERNC bool           thread_pool_make_task(thread_pool_t pool, void * param, task_handler_t handler);

//销毁线程池
EXTERNC void           thread_pool_destory(thread_pool_t pool);

//打印线程池状态报告
EXTERNC char *         thread_pool_print_report(thread_pool_t pool);
#endif
