#define _GNU_SOURCE

#include <stdlib.h>
#include <stdbool.h>
#include <sched.h>

#include "thread_pool.h"

// Initialize task queue
int task_queue_init(task_queue_t *queue, int num_priorities)
{
    queue->queues = calloc(num_priorities, sizeof(task_t *));
    if (!queue->queues)
    {
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
        free(queue->queues);
        return -1;
    }

    return 0;
}

// Add task to priority queue
int task_queue_push(task_queue_t *queue, task_t *task)
{
    if (task->priority < 0 || task->priority >= queue->num_priorities)
    {
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    // Insert at head of priority queue
    task->next = queue->queues[task->priority];
    queue->queues[task->priority] = task;

    atomic_fetch_add(&queue->size, 1);
    atomic_fetch_add(&queue->total_tasks, 1);

    pthread_cond_signal(&queue->condition);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

// Pop task from highest priority queue
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

    // Find highest priority non-empty queue
    for (int i = queue->num_priorities - 1; i >= 0; i--)
    {
        if (queue->queues[i])
        {
            task = queue->queues[i];
            queue->queues[i] = task->next;
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

// Try to pop task without blocking
task_t *task_queue_try_pop(task_queue_t *queue)
{
    if (pthread_mutex_trylock(&queue->mutex) != 0)
    {
        return NULL;
    }

    task_t *task = NULL;

    if (atomic_load(&queue->size) > 0)
    {
        // Find highest priority non-empty queue
        for (int i = queue->num_priorities - 1; i >= 0; i--)
        {
            if (queue->queues[i])
            {
                task = queue->queues[i];
                queue->queues[i] = task->next;
                atomic_fetch_sub(&queue->size, 1);
                break;
            }
        }
    }

    pthread_mutex_unlock(&queue->mutex);
    return task;
}

// Worker stealing implementation
task_t *steal_task(thread_pool_t *pool, int worker_id)
{
    int num_workers = atomic_load(&pool->active_threads);

    // Try to steal from other workers' local queues
    for (int i = 1; i < num_workers; i++)
    {
        int target = (worker_id + i) % num_workers;

        // Try to acquire lock on target queue
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

// Worker thread function
void *worker_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;
    int worker_id = atomic_fetch_add(&pool->round_robin_index, 1) % pool->max_threads;

    // Set CPU affinity if specified
    if (pool->worker_stats[worker_id].cpu_affinity >= 0)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(pool->worker_stats[worker_id].cpu_affinity, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

    struct timespec idle_start, idle_end, task_start, task_end;

    while (!atomic_load(&pool->shutdown))
    {
        task_t *task = NULL;

        clock_gettime(CLOCK_MONOTONIC, &idle_start);

        // Try local queue first (work stealing)
        if (pool->local_queues)
        {
            task = task_queue_try_pop(&pool->local_queues[worker_id]);
        }

        // Try global queue
        if (!task)
        {
            task = task_queue_pop(&pool->task_queue);
        }

        // Try work stealing
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

        // Update idle time statistics
        long idle_ns = (idle_end.tv_sec - idle_start.tv_sec) * 1000000000L +
                       (idle_end.tv_nsec - idle_start.tv_nsec);
        atomic_fetch_add(&pool->worker_stats[worker_id].idle_time_ns, idle_ns);

        // Execute task
        clock_gettime(CLOCK_MONOTONIC, &task_start);
        task->function(task->argument);
        clock_gettime(CLOCK_MONOTONIC, &task_end);

        // Update execution statistics
        long exec_ns = (task_end.tv_sec - task_start.tv_sec) * 1000000000L +
                       (task_end.tv_nsec - task_start.tv_nsec);

        atomic_fetch_add(&pool->worker_stats[worker_id].tasks_executed, 1);
        atomic_fetch_add(&pool->worker_stats[worker_id].total_execution_time_ns, exec_ns);
        atomic_fetch_add(&pool->total_tasks_completed, 1);

        pool->worker_stats[worker_id].last_task_end = task_end;

        free(task);
    }

    return NULL;
}

// Create thread pool
thread_pool_t *thread_pool_create(int num_threads, int min_threads, int max_threads,
                                  bool enable_work_stealing, int num_priorities)
{
    thread_pool_t *pool = calloc(1, sizeof(thread_pool_t));
    if (!pool)
    {
        return NULL;
    }

    pool->num_threads = num_threads;
    pool->min_threads = min_threads;
    pool->max_threads = max_threads;
    atomic_init(&pool->active_threads, num_threads);
    atomic_init(&pool->shutdown, false);
    atomic_init(&pool->immediate_shutdown, false);
    atomic_init(&pool->total_tasks_submitted, 0);
    atomic_init(&pool->total_tasks_completed, 0);
    atomic_init(&pool->round_robin_index, 0);

    clock_gettime(CLOCK_MONOTONIC, &pool->start_time);

    // Initialize main task queue
    if (task_queue_init(&pool->task_queue, num_priorities) != 0)
    {
        free(pool);
        return NULL;
    }

    // Initialize work-stealing queues
    if (enable_work_stealing)
    {
        pool->local_queues = calloc(max_threads, sizeof(task_queue_t));
        pool->queue_locks = calloc(max_threads, sizeof(atomic_int));

        if (!pool->local_queues || !pool->queue_locks)
        {
            free(pool->local_queues);
            free(pool->queue_locks);
            free(pool);
            return NULL;
        }

        for (int i = 0; i < max_threads; i++)
        {
            task_queue_init(&pool->local_queues[i], num_priorities);
            atomic_init(&pool->queue_locks[i], 0);
        }
    }

    // Allocate threads and statistics
    pool->threads = calloc(max_threads, sizeof(pthread_t));
    pool->worker_stats = calloc(max_threads, sizeof(worker_stats_t));

    if (!pool->threads || !pool->worker_stats)
    {
        free(pool->threads);
        free(pool->worker_stats);
        free(pool);
        return NULL;
    }

    // Initialize worker statistics
    for (int i = 0; i < max_threads; i++)
    {
        atomic_init(&pool->worker_stats[i].tasks_executed, 0);
        atomic_init(&pool->worker_stats[i].total_execution_time_ns, 0);
        atomic_init(&pool->worker_stats[i].idle_time_ns, 0);
        pool->worker_stats[i].cpu_affinity = -1; // No affinity by default
    }

    pthread_mutex_init(&pool->resize_mutex, NULL);

    // Create worker threads
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
        {
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

// Submit task to thread pool
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

    // Load balancing: distribute tasks among local queues
    if (pool->local_queues)
    {
        int target_queue = atomic_fetch_add(&pool->round_robin_index, 1) %
                           atomic_load(&pool->active_threads);

        if (task_queue_push(&pool->local_queues[target_queue], task) == 0)
        {
            atomic_fetch_add(&pool->total_tasks_submitted, 1);
            return 0;
        }
    }

    // Fallback to global queue
    if (task_queue_push(&pool->task_queue, task) == 0)
    {
        atomic_fetch_add(&pool->total_tasks_submitted, 1);
        return 0;
    }

    free(task);
    return -1;
}

// Set CPU affinity for worker thread
int thread_pool_set_affinity(thread_pool_t *pool, int worker_id, int cpu_id)
{
    if (worker_id < 0 || worker_id >= pool->max_threads)
    {
        return -1;
    }

    pool->worker_stats[worker_id].cpu_affinity = cpu_id;

    // Apply immediately if thread is running
    if (worker_id < pool->num_threads)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        return pthread_setaffinity_np(pool->threads[worker_id], sizeof(cpu_set_t), &cpuset);
    }

    return 0;
}

// Get thread pool statistics
void thread_pool_statistics(thread_pool_t *pool)
{
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    long uptime_ns = (current_time.tv_sec - pool->start_time.tv_sec) * 1000000000L +
                     (current_time.tv_nsec - pool->start_time.tv_nsec);

    printf("=== Thread Pool Statistics ===\n");
    printf("Uptime: %.3f seconds\n", uptime_ns / 1e9);
    printf("Active threads: %d\n", atomic_load(&pool->active_threads));
    printf("Tasks submitted: %ld\n", atomic_load(&pool->total_tasks_submitted));
    printf("Tasks completed: %ld\n", atomic_load(&pool->total_tasks_completed));
    printf("Tasks pending: %d\n", atomic_load(&pool->task_queue.size));

    long total_tasks_executed = 0;
    long total_execution_time = 0;
    long total_idle_time = 0;

    printf("\nPer-worker statistics:\n");
    for (int i = 0; i < pool->num_threads; i++)
    {
        long tasks = atomic_load(&pool->worker_stats[i].tasks_executed);
        long exec_time = atomic_load(&pool->worker_stats[i].total_execution_time_ns);
        long idle_time = atomic_load(&pool->worker_stats[i].idle_time_ns);

        total_tasks_executed += tasks;
        total_execution_time += exec_time;
        total_idle_time += idle_time;

        printf("  Worker %d: %ld tasks, %.3f ms avg exec, %.1f%% idle\n",
               i, tasks,
               tasks > 0 ? (exec_time / 1e6) / tasks : 0,
               uptime_ns > 0 ? (idle_time * 100.0) / uptime_ns : 0);
    }

    printf("\nOverall performance:\n");
    printf("  Total tasks executed: %ld\n", total_tasks_executed);
    printf("  Average execution time: %.3f ms\n",
           total_tasks_executed > 0 ? (total_execution_time / 1e6) / total_tasks_executed : 0);
    printf("  Throughput: %.1f tasks/second\n",
           uptime_ns > 0 ? (total_tasks_executed * 1e9) / uptime_ns : 0);
}

// Dynamic thraed pool resizing
int thread_pool_resize(thread_pool_t *pool, int new_size)
{
    if (new_size < pool->min_threads || new_size > pool->max_threads)
    {
        return -1;
    }

    pthread_mutex_lock(&pool->resize_mutex);

    int current_size = atomic_load(&pool->active_threads);

    if (new_size > current_size)
    {
        // Add threads
        for (int i = current_size; i < new_size; i++)
        {
            if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
            {
                pthread_mutex_unlock(&pool->resize_mutex);
                return -1;
            }
        }
        atomic_store(&pool->active_threads, new_size);
    }
    else if (new_size < current_size)
    {
        // Remove threads (they will exit naturally when checking shutdown flag)
        atomic_store(&pool->active_threads, new_size);

        // Join excess threads
        for (int i = new_size; i < current_size; i++)
        {
            pthread_join(pool->threads[i], NULL);
        }
    }

    pool->num_threads = new_size;
    pthread_mutex_unlock(&pool->resize_mutex);

    return 0;
}

// Destroy thraed pool
void thread_pool_destroy(thread_pool_t *pool)
{
    if (!pool)
    {
        return;
    }

    // Signal shutdown
    atomic_store(&pool->shutdown, true);

    // Flag every queue as shutting down and wake all waiters, so workers parked
    // in task_queue_pop return NULL and exit instead of blocking forever.
    atomic_store(&pool->task_queue.shutdown, true);
    pthread_mutex_lock(&pool->task_queue.mutex);
    pthread_cond_broadcast(&pool->task_queue.condition);
    pthread_mutex_unlock(&pool->task_queue.mutex);

    if (pool->local_queues)
    {
        for (int i = 0; i < pool->max_threads; i++)
        {
            atomic_store(&pool->local_queues[i].shutdown, true);
            pthread_mutex_lock(&pool->local_queues[i].mutex);
            pthread_cond_broadcast(&pool->local_queues[i].condition);
            pthread_mutex_unlock(&pool->local_queues[i].mutex);
        }
    }

    // Wait for threads to finish
    for (int i = 0; i < pool->num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    // Cleanup
    pthread_mutex_destroy(&pool->task_queue.mutex);
    pthread_cond_destroy(&pool->task_queue.condition);
    pthread_mutex_destroy(&pool->resize_mutex);

    // Free local queues
    if (pool->local_queues)
    {
        for (int i = 0; i < pool->max_threads; i++)
        {
            pthread_mutex_destroy(&pool->local_queues[i].mutex);
            pthread_cond_destroy(&pool->local_queues[i].condition);
            free(pool->local_queues[i].queues);
        }
        free(pool->local_queues);
        free(pool->queue_locks);
    }

    free(pool->task_queue.queues);
    free(pool->threads);
    free(pool->worker_stats);
    free(pool);
}