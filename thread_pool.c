#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include "thread_pool.h"

typedef struct thread_pool_task_
{
	void *					  param;
	task_handler_t			  handler;
	struct thread_pool_task_ *next;
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
	struct thread_ * prev;
	struct thread_ * next;
}*thread_t;

typedef struct thread_pool_
{
	thread_t	          threads;				//线程
	unsigned short        max_thread_count;		//最大线程数量
	unsigned short        min_thread_count;		//最低线程数量
	unsigned short		  current_thread_count;	//当前线程数量
	unsigned short        idle_thread_count;    //空闲线程数量

	pthread_mutex_t       thread_mtx;			//线程数据的互斥

	pthread_mutex_t       tasks_mtx;			//任务数据的互斥
	pthread_cond_t        tasks_cond;			//任务数据的条件变量

	thread_pool_task_t	  waiting_tasks;	    //等待执行的任务
	thread_pool_task_t    waiting_tasks_tail;	//等待执行任务的尾部，用于快速插入

	thread_pool_task_t    idle_tasks;			//空闲任务列表
	thread_pool_task_t    idle_tasks_tail;		//空闲任务尾部，用于快速插入

	unsigned short        waiting_tasks_count;  //等待执行任务数量
	unsigned short        idle_tasks_count;		//空闲任务数量
};

typedef struct thread_param_
{
	thread_pool_t pool;
	thread_t      thread;
}*thread_param_t;

static inline void* _thread_pool_proc(void * args);

//停止一个县城
static inline void     _thread_pool_stop_thread(thread_t thread)
{
	if (!thread)
		return;

	thread->run = false;
	pthread_join(thread->handle, NULL);
	free(thread);
}

//清理线程
static inline void   _thread_pool_free_thread(thread_pool_t pool)
{
	thread_t thread = pool->threads;
	while (thread)
	{
		thread_t cur = thread;
		thread = cur->next;
		_thread_pool_stop_thread(cur);
	}
	pool->threads = NULL;
	pool->idle_thread_count = 0;
	pool->current_thread_count = 0;
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
	thread->status = thread_status_idle;
	int ret = pthread_create(&thread->handle, NULL, thread_proc, param);
	if (ret != 0)
		goto clean;

	return thread;
clean:
	if (param)
		free(param);

	_thread_pool_stop_thread(thread);
	return NULL;
}

//初始化线程链表
static inline bool _thread_pool_init_threads(thread_pool_t pool, unsigned short count)
{
	pthread_mutex_lock(&pool->thread_mtx);
	int i = 0;
	bool success = false;
	int final_count = (pool->current_thread_count + count) > pool->max_thread_count
		? (pool->max_thread_count - pool->current_thread_count) : count;

	for (; i < final_count; i++)
	{
		thread_t thread = _thread_pool_create_thread(pool, _thread_pool_proc);
		if (!thread)
			goto unlock;

		if (!pool->threads)
			pool->threads = thread;
		else
			pool->threads->prev->next = thread;

		pool->threads->prev = thread;
		pool->current_thread_count++;
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
	return  pool->waiting_tasks_count - pool->idle_thread_count;
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
	task->next = NULL;
	pthread_mutex_lock(&pool->tasks_mtx);
	if (!pool->idle_tasks)
		pool->idle_tasks = task;
	else
		pool->idle_tasks_tail->next = task;

	pool->idle_tasks_count++;
	pool->idle_tasks_tail = task;
	pthread_mutex_unlock(&pool->tasks_mtx);
}

//获取等待执行的任务
static inline bool  _thread_pool_get_task(thread_pool_t pool, thread_t thread, thread_pool_task_t * task)
{
	bool success = false;
	pthread_mutex_lock(&pool->tasks_mtx);
	if (pool->waiting_tasks_count <= 0)
	{
		pool->idle_thread_count++;
		thread->status = thread_status_idle;
		pthread_cond_wait(&pool->tasks_cond, &pool->tasks_mtx);
		goto unlock;
	}

	*task = pool->waiting_tasks;
	pool->waiting_tasks = (*task)->next;
	if (!pool->waiting_tasks)
		pool->waiting_tasks_tail = NULL;


	pool->waiting_tasks_count--;
	pool->idle_thread_count--;
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
thread_pool_t thread_pool_create(unsigned short min_count, unsigned short max_count)
{
	if (min_count <= 0 || max_count > 16)
		return NULL;

	thread_pool_t  pool = (thread_pool_t)calloc(1, sizeof(struct thread_pool_));
	if (!pool)
		goto clean;

	pool->max_thread_count = max_count;
	pool->min_thread_count = min_count;

	pthread_mutex_init(&pool->thread_mtx,NULL);
	pthread_mutex_init(&pool->tasks_mtx, NULL);
	pthread_cond_init(&pool->tasks_cond, NULL);

	if (!_thread_pool_init_threads(pool, min_count))
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
	if (pool->idle_tasks_count == 0)
	{
		task = (thread_pool_task_t)calloc(1, sizeof(struct thread_pool_task_));
		if (!task)
			goto unlock;
	}
	else
	{
		
		task = pool->idle_tasks;
		pool->idle_tasks = task->next;
		pool->idle_tasks_count--;
		memset(task, 0, sizeof(struct thread_pool_task_));
	}

	task->param = param;
	task->handler = handler;

	if (!pool->waiting_tasks)
		pool->waiting_tasks = task;
	else
		pool->waiting_tasks_tail->next = task;

	pool->waiting_tasks_count++;
	pool->waiting_tasks_tail = task;
	pthread_cond_signal(&pool->tasks_cond);
unlock:
	pthread_mutex_unlock(&pool->tasks_mtx);
	return success;
}
 
//清理等待任务列表
static inline void _thread_pool_free_waiting_task(thread_pool_t pool)
{
	pthread_mutex_lock(&pool->tasks_mtx);
	thread_pool_task_t task = pool->waiting_tasks;
	while (task)
	{
		thread_pool_task_t cur = task;
		free(cur);
		task = cur->next;
	}

	pool->waiting_tasks = NULL;
	pool->waiting_tasks_tail = NULL;
	pool->waiting_tasks_count = 0;
	pthread_mutex_unlock(&pool->tasks_mtx);

}


static inline void _thread_pool_free_idle_task(thread_pool_t pool)
{
	pthread_mutex_lock(&pool->tasks_mtx);
	thread_pool_task_t task = pool->idle_tasks;
	while (task)
	{
		thread_pool_task_t cur = task;
		free(cur);
		task = cur->next;
	}

	pool->idle_tasks = NULL;
	pool->idle_tasks_tail = NULL;
	pool->idle_tasks_count = 0;
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

//收缩线程数量和空闲任务链表
void  thread_pool_shrink(thread_pool_t pool)
{
	thread_t thread = NULL;
	thread_t * idle_threads = NULL;
	unsigned short idle_thread_count = 0;

	pthread_mutex_lock(&pool->thread_mtx);
	if (pool->current_thread_count <= pool->min_thread_count
		|| pool->idle_thread_count <= 0)
		goto unlock;

	thread = pool->threads;
	idle_threads = calloc(pool->max_thread_count, sizeof(thread_t));
	if (!idle_threads)
		goto unlock;

	while (thread)
	{
		if (thread->status == thread_status_idle)
		{
			thread_t cur_thread = thread;
			thread = cur_thread->next;
			thread->prev = cur_thread->prev;

			pool->current_thread_count--;

			_thread_pool_stop_thread(thread);
			idle_threads[idle_thread_count] = cur_thread;
			idle_thread_count++;
		}
	}
	//将备pthread_cond_wait 阻塞的所有空闲线程唤醒
	pthread_cond_broadcast(&pool->tasks_cond);
unlock:
	pthread_mutex_unlock(&pool->thread_mtx);
	if (idle_threads)
	{
		int i = 0;
		for (; i < idle_thread_count; i++)
			_thread_pool_stop_thread(idle_threads[i]);

		free(idle_threads);
	}
}

//打印线程池状态报告
char * thread_pool_print_report(thread_pool_t pool)
{
	return NULL;
}
