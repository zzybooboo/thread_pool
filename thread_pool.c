#include <stdlib.h>
#include <stdbool.h>
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
	thread_t	          threads;
	unsigned short        max_thread_count;
	unsigned short        min_thread_count;
	unsigned short		  current_thread_count;
	unsigned short        idle_thread_count;

	pthread_mutex_t       thread_mtx;

	pthread_mutex_t       tasks_mtx;
	pthread_cond_t        tasks_cond;

	thread_pool_task_t	  tasks_root;
	thread_pool_task_t    tasks_tail;
	unsigned int          tasks_counts;
};

typedef struct thread_param_
{
	thread_pool_t pool;
	thread_t      thread;
}*thread_param_t;


static inline void* _thread_pool_proc(void * args)
{
	thread_t thread = ((thread_param_t)args)->thread;
	thread_pool_t pool = ((thread_param_t)args)->pool;

	while (thread->run)
	{
		thread_pool_task_t task = NULL;
		if (!_thread_pool_get_task(pool, thread, &task))
			continue;

		(*task->handler)(task->param);
	}

	free(args);
}

static inline  short  _thread_pool_need_stretch(thread_pool_t pool)
{
	//任务数量大于 空闲线程的数量
	return  pool->tasks_counts - pool->idle_thread_count;
}

static inline bool _thread_pool_init_threads(thread_pool_t pool, unsigned short count)
{
	pthread_mutex_lock(&pool->thread_mtx);
	int i = 0;
	bool success = false;
	int final_count = (pool->current_thread_count + count) > pool->max_thread_count
		? (pool->max_thread_count - pool->current_thread_count) : count;

	for (; i < final_count; i++)
	{
		thread_t thread = _thread_pool_create_thread(pool,_thread_pool_proc);
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

static inline void  _thread_pool_stretch(thread_pool_t pool)
{
	short need_stretch = _thread_pool_need_stretch(pool);
	if (need_stretch > 0)
		_thread_pool_init_threads(pool, need_stretch);
}

static inline bool  _thread_pool_get_task(thread_pool_t pool, thread_t thread, thread_pool_task_t * task)
{
	bool success = false;
	pthread_mutex_lock(pool->tasks_mtx);
	if (pool->tasks_counts <= 0)
	{
		pool->idle_thread_count++;
		thread->status = thread_status_idle;
		pthread_cond_wait(&pool->tasks_cond, &pool->tasks_mtx);
		goto unlock;
	}

	*task = pool->tasks_root;
	pool->tasks_root = (*task)->next;
	if (!pool->tasks_root)
		pool->tasks_tail = NULL;


	pool->tasks_counts--;
	pool->idle_thread_count--;
	thread->status = thread_status_busy;
	success = true;

unlock:
	pthread_mutex_unlock(&pool->tasks_mtx);
	if (success)
		_thread_pool_stretch(pool);

	return success;
}

static inline void     _thread_pool_stop_thread(thread_t thread)
{
	if (!thread)
		return;

	thread->run = false;
	pthread_join(thread->handle, NULL);
}

static inline void   _thread_pool_free_thread(thread_t thread)
{
	_thread_pool_stop_thread(thread);
	free(thread);
}

static inline thread_t _thread_pool_create_thread(thread_pool_t pool, thread_proc_t thread_proc)
{
	thread_param_t param = NULL;
	thread_t thread = (thread_t)calloc(1, sizeof(struct thread_));
	if (!thread)
		return false;

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

	_thread_pool_free_thread(thread);
	return NULL;
}

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

clean:
	thread_pool_destory(pool);
	return NULL;
}

bool thread_pool_make_task(thread_pool_t pool, void * param, task_handler_t handler)
{
	if (!pool)
		return false;

	thread_pool_task_t task = (thread_pool_task_t)calloc(1, sizeof(struct thread_pool_task_));
	if (!task)
		return NULL;

	task->param = param;
	task->handler = handler;
	
	pthread_mutex_lock(&pool->tasks_mtx);

	if (!pool->tasks_root)
		pool->tasks_root = task;
	else
		pool->tasks_tail->next = task;
	
	pool->tasks_counts++;
	pool->tasks_tail = task;

	pthread_cond_signal(&pool->tasks_cond);
	pthread_mutex_unlock(&pool->tasks_mtx);
	return true;
}

void  thread_pool_destory(thread_pool_t pool)
{
	if (!pool)
		return;

	//关闭所有线程
	//释放所有线程句柄
	//释放任务列表
}

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
			_thread_pool_free_thread(idle_threads[i]);

		free(idle_threads);
	}
}


char * thread_pool_print_report(thread_pool_t pool)
{
	return NULL;
}