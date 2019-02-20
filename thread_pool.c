#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "list.h"
#include <pthread.h>
#include "thread_pool.h"

typedef struct thread_pool_task_
{
	void *			 param;
	task_handler_t	 handler;
	struct list_head node;
}*thread_pool_task_t;


typedef void * (*thread_proc_t)(void *);

typedef enum thread_status_
{
	thread_status_idle = 0,
	thread_status_busy
}thread_status_t;

typedef struct thread_
{
	bool			 run;
	thread_status_t  status;
	pthread_t		 handle;
	struct list_head node;
}*thread_t;

typedef struct thread_pool_
{
	struct list_head		 threads;				//线程
	pthread_mutex_t			 thread_mtx;			//线程数据的互斥

	pthread_mutex_t			 tasks_mtx;			    //任务数据的互斥
	pthread_cond_t			 tasks_cond;			//任务数据的条件变量

	struct list_head 	     waiting_tasks;	        //等待执行的任务
	struct list_head	     idle_tasks;			//空闲任务列表

	struct thread_pool_info_ info;				//线程池信息
	
};

typedef struct thread_param_
{
	thread_pool_t pool;
	thread_t      thread;
}*thread_param_t;

static inline void* _thread_pool_proc(void * args);

//停止并释放一个线程
static inline void     _thread_pool_stop_thread(thread_t thread)
{
	if (!thread)
		return;

	thread->run = false;
	pthread_join(thread->handle, NULL);
}

//清理线程
static inline void   _thread_pool_free_thread(thread_pool_t pool)
{
	pthread_mutex_lock(&pool->thread_mtx);

	thread_t thread = list_first_entry(&pool->threads, struct thread_, node);
	while (thread)
	{
		thread->run = false;
		if (thread->node.next == (&pool->threads))
			break;

		thread = list_entry(thread->node.next, struct thread_, node);
	}

	while (!list_empty(&pool->threads))
	{
		thread = list_first_entry(&pool->threads, struct thread_, node);

		list_del(&thread->node);

		_thread_pool_stop_thread(thread);
		free(thread);
	}

	pool->info.idle_thread_count = 0;
	pool->info.current_thread_count = 0;
	pthread_mutex_unlock(&pool->thread_mtx);
}

//创建线程
static inline thread_t _thread_pool_create_thread(thread_pool_t pool, thread_proc_t thread_proc)
{
	thread_param_t param = NULL;
	thread_t thread = (thread_t)calloc(1, sizeof(struct thread_));
	if (!thread)
		return NULL;

	param = (thread_param_t)calloc(1, sizeof(struct thread_param_));
	if (!param)
		goto clean;

	param->pool = pool;
	param->thread = thread;

	thread->run = true;
	thread->status = thread_status_busy;
	int ret = pthread_create(&thread->handle, NULL, thread_proc, param);
	if (ret != 0)
		goto clean;

	return thread;
clean:
	if (param)
		free(param);

	if (thread)
	{
		_thread_pool_stop_thread(thread);
		free(thread);
	}
		
	return NULL;
}

//初始化线程链表
static inline bool _thread_pool_init_threads(thread_pool_t pool, unsigned short count)
{
	pthread_mutex_lock(&pool->thread_mtx);
	int i = 0;
	bool success = false;
	int final_count = (pool->info.current_thread_count + count) > pool->info.max_thread_count
		? (pool->info.max_thread_count - pool->info.current_thread_count) : count;

	for (; i < final_count; i++)
	{
		thread_t thread = _thread_pool_create_thread(pool, _thread_pool_proc);
		if (!thread)
			goto unlock;

		list_add(&thread->node, &pool->threads);
		pool->info.current_thread_count++;
	}

	success = true;
unlock:
	pthread_mutex_unlock(&pool->thread_mtx);
	return success;
}

//是否需要对线程链表进行扩容
static inline  short  _thread_pool_need_stretch(thread_pool_t pool)
{
	//任务数量大于 空闲线程的数量
	return  pool->info.waiting_tasks_count - pool->info.idle_thread_count;
}

//对线程链表进行扩容
static inline void  _thread_pool_stretch(thread_pool_t pool)
{
	short need_stretch = _thread_pool_need_stretch(pool);
	if (need_stretch > 0)
		_thread_pool_init_threads(pool, need_stretch);
}

//插入空闲任务
static inline void  _thread_pool_add_idle_task(thread_pool_t pool, thread_pool_task_t task)
{
	
	pthread_mutex_lock(&pool->tasks_mtx);

	list_add(&task->node, &pool->idle_tasks);
	pool->info.idle_tasks_count++;

	pthread_mutex_unlock(&pool->tasks_mtx);
}

//获取等待执行的任务
static inline bool  _thread_pool_get_task(thread_pool_t pool, thread_t thread, thread_pool_task_t * task)
{
	bool success = false;
	pthread_mutex_lock(&pool->tasks_mtx);
	//如果等待执行任务列表为空
	if (list_empty(&pool->waiting_tasks))
	{
		pool->info.idle_thread_count++;
		thread->status = thread_status_idle;
		pthread_cond_wait(&pool->tasks_cond, &pool->tasks_mtx);
		pool->info.idle_thread_count--;
	}

	if (!thread->run)
		goto unlock;

	*task = list_first_entry(&pool->waiting_tasks, struct thread_pool_task_, node);
	list_del(&(*task)->node);
	pool->info.waiting_tasks_count--;
	thread->status = thread_status_busy;

	success = true;
unlock:
	pthread_mutex_unlock(&pool->tasks_mtx);
	if (success)
		_thread_pool_stretch(pool);

	return success;
}

//线程函数
static inline void * _thread_pool_proc(void * args)
{
	thread_t thread = ((thread_param_t)args)->thread;
	thread_pool_t pool = ((thread_param_t)args)->pool;

	while (thread->run)
	{
		thread_pool_task_t task = NULL;
		if (!_thread_pool_get_task(pool, thread, &task))
			continue;

		(*task->handler)(task->param);

		//将task 插入到 空闲列表
		_thread_pool_add_idle_task(pool, task);
	}
	free(args);
}

//创建线程池 最少线程数量，最大线程数量
thread_pool_t thread_pool_create(unsigned short min_thread_count, unsigned short max_thread_count)
{
	if (min_thread_count <= 0)
		return NULL;

	thread_pool_t  pool = (thread_pool_t)calloc(1, sizeof(struct thread_pool_));
	if (!pool)
		goto clean;

	INIT_LIST_HEAD(&pool->threads);
	INIT_LIST_HEAD(&pool->idle_tasks);
	INIT_LIST_HEAD(&pool->waiting_tasks);
	
	pool->info.max_thread_count = max_thread_count;
	pool->info.min_thread_count = min_thread_count;

	pthread_mutex_init(&pool->thread_mtx,NULL);
	pthread_mutex_init(&pool->tasks_mtx, NULL);
	pthread_cond_init(&pool->tasks_cond, NULL);

	if (!_thread_pool_init_threads(pool, min_thread_count))
		goto clean;

	return pool;
clean:
	thread_pool_destory(pool);
	return NULL;
}

//添加待执行任务
bool thread_pool_make_task(thread_pool_t pool, void * param, task_handler_t handler)
{
	if (!pool)
		return false;

	bool success = false;
	pthread_mutex_lock(&pool->tasks_mtx);
	thread_pool_task_t task = NULL;
	//没有空闲任务 则创建一个申请一个新的任务对象
	if (list_empty(&pool->idle_tasks))
	{
		task = (thread_pool_task_t)calloc(1, sizeof(struct thread_pool_task_));
		if (!task)
			goto unlock;

	}
	else
	{
		//从空闲任务列表取出头部
		task = list_first_entry(&pool->idle_tasks, struct thread_pool_task_, node);
		list_del(&task->node);
		pool->info.idle_tasks_count--;
	}

	task->param = param;
	task->handler = handler;
	
	//插入到待执行列表
	list_add(&task->node, &pool->waiting_tasks);
	pool->info.waiting_tasks_count++;
	pthread_cond_signal(&pool->tasks_cond);
unlock:
	pthread_mutex_unlock(&pool->tasks_mtx);
	return success;
}
 
//清理等待任务列表
static inline void _thread_pool_free_waiting_task(thread_pool_t pool)
{
	pthread_mutex_lock(&pool->tasks_mtx);
	
	while (!list_empty(&pool->waiting_tasks))
	{
		thread_pool_task_t task = list_first_entry(&pool->waiting_tasks, struct thread_pool_task_, node);
		list_del(&task->node);
		free(task);
		pool->info.waiting_tasks_count--;
	}
	pthread_mutex_unlock(&pool->tasks_mtx);
}

//清理空闲任务列表
static inline void _thread_pool_free_idle_task(thread_pool_t pool)
{
	pthread_mutex_lock(&pool->tasks_mtx);
	while (!list_empty(&pool->idle_tasks))
	{
		thread_pool_task_t task = list_first_entry(&pool->idle_tasks, struct thread_pool_task_, node);
		list_del(&task->node);
		free(task);
		pool->info.idle_tasks_count--;
	}
	pthread_mutex_unlock(&pool->tasks_mtx);
	
}

//线程池销毁
void  thread_pool_destory(thread_pool_t pool)
{
	if (!pool)
		return;

	_thread_pool_free_thread(pool);
	//释放任务列表
	_thread_pool_free_idle_task(pool);
	_thread_pool_free_waiting_task(pool);

	pthread_mutex_destroy(&pool->thread_mtx);
	pthread_mutex_destroy(&pool->tasks_mtx);
	pthread_cond_destroy(&pool->tasks_cond);
	free(pool);
}

static inline void _thread_pool_free_idle_thread(thread_pool_t pool)
{
	thread_t * idle_threads = NULL;
	thread_t thread = NULL, next_thread = NULL;
	unsigned short idle_thread_count = 0;

	pthread_mutex_lock(&pool->thread_mtx);
	if (pool->info.current_thread_count <= pool->info.min_thread_count
		|| pool->info.idle_thread_count <= 0)
		goto unlock;

	
	idle_threads = calloc(pool->info.max_thread_count, sizeof(thread_t));
	if (!idle_threads)
		goto unlock;

	thread = list_first_entry(&pool->threads, struct thread_, node);;
	while (thread)
	{
		if (pool->info.current_thread_count <= pool->info.min_thread_count)
			break;

		
		if (thread->node.next == (&pool->threads))
			break;

		next_thread = list_entry(thread->node.next, struct thread_, node);
		list_del(&thread->node);
		if (thread->status == thread_status_idle)
		{
			thread->run = false;
			idle_threads[idle_thread_count] = thread;
			idle_thread_count++;
			pool->info.current_thread_count--;
		}
		thread = next_thread;
	}

	printf("waiting free idle thread count:%d\n", idle_thread_count);
	//将备pthread_cond_wait 阻塞的所有空闲线程唤醒
	pthread_cond_broadcast(&pool->tasks_cond);
unlock:
	pthread_mutex_unlock(&pool->thread_mtx);
	if (idle_threads)
	{
		int i = 0;
		for (; i < idle_thread_count; i++)
		{
			_thread_pool_stop_thread(idle_threads[i]);
			free(idle_threads[i]);
		}
		free(idle_threads);
	}
}

//收缩线程数量和空闲任务链表
void  thread_pool_shrink(thread_pool_t pool)
{
	_thread_pool_free_idle_task(pool);
	_thread_pool_free_idle_thread(pool);
}

//打印线程池状态报告
thread_pool_info_t thread_pool_info(thread_pool_t pool)
{
	if (!pool)
		return NULL;

	return &pool->info;
}
