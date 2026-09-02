#include <stdlib.h>
#include <stdbool.h>
#include <sched.h>

#include "thread_pool.h"

int task_queue_init(task_queue_t *queue, int num_priorities)
{
    queue->queues = calloc(num_priorities, sizeof(task_t *));
    if (!queue->queues)
    {
        return -1;
    }

    // Per-priority tail pointers so pushes append in O(1) at the tail while pops
    // dequeue at the head, giving FIFO ordering within a priority level.
    queue->tails = calloc(num_priorities, sizeof(task_t *));
    if (!queue->tails)
    {
        free(queue->queues);
        return -1;
    }

    queue->num_priorities = num_priorities;
    atomic_init(&queue->size, 0);
    atomic_init(&queue->total_tasks, 0);
    atomic_init(&queue->shutdown, false);

    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
    {
        free(queue->queues);
        return -1;
    }

    if (pthread_cond_init(&queue->condition, NULL) != 0)
    {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->tails);
        free(queue->queues);
        return -1;
    }

    return 0;
}

// Destroy a task queue's synchronization primitives and free its priority
// arrays. Does not free queued task_t nodes (see task_queue_drain) or the queue
// struct itself.
static void task_queue_destroy(task_queue_t *queue)
{
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->condition);
    free(queue->queues);
    free(queue->tails);
}

// Free every task_t still queued (without executing it) so teardown doesn't leak
// the nodes. Callers must ensure no worker touches the queue concurrently.
static void task_queue_drain(task_queue_t *queue)
{
    if (!queue->queues)
    {
        return;
    }

    for (int i = 0; i < queue->num_priorities; i++)
    {
        task_t *task = queue->queues[i];
        while (task)
        {
            task_t *next = task->next;
            // NOTE: task->argument (e.g. an offload_task_t) is intentionally not
            // freed here. We do not own its lifetime and the connection it refers
            // to may already be gone, so we must not run the handler. Freeing only
            // the node stops the task_t leak; any arg still owned by the task
            // leaks by design during a hard shutdown.
            free(task);
            task = next;
        }
        queue->queues[i] = NULL;
        if (queue->tails)
        {
            queue->tails[i] = NULL;
        }
    }
    atomic_store(&queue->size, 0);
}

int task_queue_push(task_queue_t *queue, task_t *task)
{
    if (task->priority < 0 || task->priority >= queue->num_priorities)
    {
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    // Append at the tail of the priority queue so tasks are served FIFO.
    task->next = NULL;
    if (queue->queues[task->priority] == NULL)
    {
        queue->queues[task->priority] = task;
        queue->tails[task->priority] = task;
    }
    else
    {
        queue->tails[task->priority]->next = task;
        queue->tails[task->priority] = task;
    }

    atomic_fetch_add(&queue->size, 1);
    atomic_fetch_add(&queue->total_tasks, 1);

    pthread_cond_signal(&queue->condition);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

task_t *task_queue_pop(task_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);

    while (atomic_load(&queue->size) == 0 && !atomic_load(&queue->shutdown))
    {
        pthread_cond_wait(&queue->condition, &queue->mutex);
    }

    // Woken for shutdown with nothing to do: return NULL so the worker loop can
    // observe pool->shutdown and exit cleanly.
    if (atomic_load(&queue->size) == 0)
    {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    task_t *task = NULL;

    // Find highest priority non-empty queue (dequeue from the head for FIFO)
    for (int i = queue->num_priorities - 1; i >= 0; i--)
    {
        if (queue->queues[i])
        {
            task = queue->queues[i];
            queue->queues[i] = task->next;
            if (queue->queues[i] == NULL)
            {
                queue->tails[i] = NULL;
            }
            task->next = NULL;
            break;
        }
    }

    if (task)
    {
        atomic_fetch_sub(&queue->size, 1);
    }

    pthread_mutex_unlock(&queue->mutex);
    return task;
}

task_t *task_queue_try_pop(task_queue_t *queue)
{
    if (pthread_mutex_trylock(&queue->mutex) != 0)
    {
        return NULL;
    }

    task_t *task = NULL;

    if (atomic_load(&queue->size) > 0)
    {
        // Find highest priority non-empty queue (dequeue from the head for FIFO)
        for (int i = queue->num_priorities - 1; i >= 0; i--)
        {
            if (queue->queues[i])
            {
                task = queue->queues[i];
                queue->queues[i] = task->next;
                if (queue->queues[i] == NULL)
                {
                    queue->tails[i] = NULL;
                }
                task->next = NULL;
                atomic_fetch_sub(&queue->size, 1);
                break;
            }
        }
    }

    pthread_mutex_unlock(&queue->mutex);
    return task;
}

task_t *steal_task(thread_pool_t *pool, int worker_id)
{
    int num_workers = pool->num_threads;

    for (int i = 1; i < num_workers; i++)
    {
        int target = (worker_id + i) % num_workers;

        int expected = 0;
        if (atomic_compare_exchange_weak(&pool->queue_locks[target], &expected, 1))
        {
            task_t *stolen_task = task_queue_try_pop(&pool->local_queues[target]);
            atomic_store(&pool->queue_locks[target], 0);

            if (stolen_task)
            {
                return stolen_task;
            }
        }
    }

    return NULL;
}

void *worker_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;
    // Cast through unsigned so a wrapped (negative) counter can never produce a
    // negative index into the worker arrays.
    int worker_id = (int)((unsigned)atomic_fetch_add(&pool->round_robin_index, 1) %
                          (unsigned)pool->capacity);

    struct timespec idle_start, idle_end, task_start, task_end;

    while (!atomic_load(&pool->shutdown))
    {
        task_t *task = NULL;

        clock_gettime(CLOCK_MONOTONIC, &idle_start);

        if (pool->local_queues)
        {
            task = task_queue_try_pop(&pool->local_queues[worker_id]);
        }

        if (!task)
        {
            task = task_queue_pop(&pool->task_queue);
        }

        if (!task && pool->local_queues)
        {
            task = steal_task(pool, worker_id);
        }

        if (!task)
        {
            if (atomic_load(&pool->immediate_shutdown))
            {
                break;
            }
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &idle_end);

        long idle_ns = (idle_end.tv_sec - idle_start.tv_sec) * 1000000000L +
                       (idle_end.tv_nsec - idle_start.tv_nsec);
        atomic_fetch_add(&pool->worker_stats[worker_id].idle_time_ns, idle_ns);

        clock_gettime(CLOCK_MONOTONIC, &task_start);
        task->function(task->argument);
        clock_gettime(CLOCK_MONOTONIC, &task_end);

        long exec_ns = (task_end.tv_sec - task_start.tv_sec) * 1000000000L +
                       (task_end.tv_nsec - task_start.tv_nsec);

        atomic_fetch_add(&pool->worker_stats[worker_id].tasks_executed, 1);
        atomic_fetch_add(&pool->worker_stats[worker_id].total_execution_time_ns, exec_ns);
        atomic_fetch_add(&pool->total_tasks_completed, 1);

        free(task);
    }

    return NULL;
}

thread_pool_t *thread_pool_create(int num_threads, bool enable_work_stealing,
                                  int num_priorities)
{
    thread_pool_t *pool = calloc(1, sizeof(thread_pool_t));
    if (!pool)
    {
        return NULL;
    }

    pool->num_threads = num_threads;
    // Arrays are sized once at creation; the pool never grows or shrinks.
    pool->capacity = num_threads;
    atomic_init(&pool->shutdown, false);
    atomic_init(&pool->immediate_shutdown, false);
    atomic_init(&pool->total_tasks_submitted, 0);
    atomic_init(&pool->total_tasks_completed, 0);
    atomic_init(&pool->round_robin_index, 0);

    clock_gettime(CLOCK_MONOTONIC, &pool->start_time);

    if (task_queue_init(&pool->task_queue, num_priorities) != 0)
    {
        free(pool);
        return NULL;
    }

    if (enable_work_stealing)
    {
        pool->local_queues = calloc(pool->capacity, sizeof(task_queue_t));
        pool->queue_locks = calloc(pool->capacity, sizeof(atomic_int));

        if (!pool->local_queues || !pool->queue_locks)
        {
            free(pool->local_queues);
            free(pool->queue_locks);
            // task_queue_init already allocated the main queue; release it too.
            task_queue_destroy(&pool->task_queue);
            free(pool);
            return NULL;
        }

        for (int i = 0; i < pool->capacity; i++)
        {
            task_queue_init(&pool->local_queues[i], num_priorities);
            atomic_init(&pool->queue_locks[i], 0);
        }
    }

    pool->threads = calloc(pool->capacity, sizeof(pthread_t));
    pool->worker_stats = calloc(pool->capacity, sizeof(worker_stats_t));

    if (!pool->threads || !pool->worker_stats)
    {
        free(pool->threads);
        free(pool->worker_stats);
        // Release everything allocated so far: the local work-stealing queues
        // (and their inner arrays / mutexes / conds), the queue-lock array, and
        // the main task queue.
        if (pool->local_queues)
        {
            for (int i = 0; i < pool->capacity; i++)
            {
                task_queue_destroy(&pool->local_queues[i]);
            }
            free(pool->local_queues);
            free(pool->queue_locks);
        }
        task_queue_destroy(&pool->task_queue);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < pool->capacity; i++)
    {
        atomic_init(&pool->worker_stats[i].tasks_executed, 0);
        atomic_init(&pool->worker_stats[i].total_execution_time_ns, 0);
        atomic_init(&pool->worker_stats[i].idle_time_ns, 0);
    }

    int created = 0;
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
        {
            // Only join the threads we actually created. thread_pool_destroy
            // joins pool->num_threads handles, so cap it at the created count to
            // avoid pthread_join on zeroed (never-created) pthread_t handles.
            pool->num_threads = created;
            thread_pool_destroy(pool);
            return NULL;
        }
        created++;
    }

    return pool;
}

int thread_pool_submit(thread_pool_t *pool, void (*function)(void *), void *argument, int priority)
{
    if (atomic_load(&pool->shutdown))
    {
        return -1;
    }

    task_t *task = malloc(sizeof(task_t));
    if (!task)
    {
        return -1;
    }

    task->function = function;
    task->argument = argument;
    task->priority = priority;
    task->next = NULL;
    gettimeofday(&task->submit_time, NULL);

    // Submit to the global queue.
    //
    // Idle workers park on the GLOBAL queue's condition variable (via
    // task_queue_pop). A task pushed only to a per-worker local queue would
    // therefore never wake a sleeping worker -- its local condvar has no waiters,
    // and a woken worker re-checks the global size and goes straight back to
    // sleep -- deadlocking submitted work behind idle threads. Until the worker
    // loop is restructured to block on a shared "work available" condition,
    // route every submission through the global queue so an idle worker is always
    // woken. Workers still probe their local queue and steal first, so this stays
    // correct; it only forgoes local-queue locality when work stealing is enabled.
    // (The non-work-stealing path, which the server uses, already took this exact
    // branch, so its behavior is unchanged.)
    if (task_queue_push(&pool->task_queue, task) == 0)
    {
        atomic_fetch_add(&pool->total_tasks_submitted, 1);
        return 0;
    }

    free(task);
    return -1;
}

void thread_pool_destroy(thread_pool_t *pool)
{
    if (!pool)
    {
        return;
    }

    atomic_store(&pool->shutdown, true);

    // Flag every queue as shutting down and wake all waiters, so workers parked
    // in task_queue_pop return NULL and exit instead of blocking forever.
    atomic_store(&pool->task_queue.shutdown, true);
    pthread_mutex_lock(&pool->task_queue.mutex);
    pthread_cond_broadcast(&pool->task_queue.condition);
    pthread_mutex_unlock(&pool->task_queue.mutex);

    if (pool->local_queues)
    {
        for (int i = 0; i < pool->capacity; i++)
        {
            atomic_store(&pool->local_queues[i].shutdown, true);
            pthread_mutex_lock(&pool->local_queues[i].mutex);
            pthread_cond_broadcast(&pool->local_queues[i].condition);
            pthread_mutex_unlock(&pool->local_queues[i].mutex);
        }
    }

    for (int i = 0; i < pool->num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    // Workers have joined, so no one else touches the queues now. Free any tasks
    // that were still queued at shutdown so their task_t nodes don't leak (we do
    // NOT execute them: their connections may already be gone).
    task_queue_drain(&pool->task_queue);
    if (pool->local_queues)
    {
        for (int i = 0; i < pool->capacity; i++)
        {
            task_queue_drain(&pool->local_queues[i]);
        }
    }

    task_queue_destroy(&pool->task_queue);

    if (pool->local_queues)
    {
        for (int i = 0; i < pool->capacity; i++)
        {
            task_queue_destroy(&pool->local_queues[i]);
        }
        free(pool->local_queues);
        free(pool->queue_locks);
    }

    free(pool->threads);
    free(pool->worker_stats);
    free(pool);
}