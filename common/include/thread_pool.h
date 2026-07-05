#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <stdio.h>

// Task structure
typedef struct task
{
  void (*function)(void *arg);
  void *argument;
  struct task *next;
  int priority;
  struct timeval submit_time;
} task_t;

typedef struct task_queue
{
  task_t **queues; // Array of priority queue
  int num_priorities;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  _Atomic(int) size;
  _Atomic(int) total_tasks;
  // Set at teardown so a blocking task_queue_pop can wake and return instead of
  // waiting forever for work that will never arrive (otherwise pthread_join in
  // thread_pool_destroy deadlocks against idle workers).
  _Atomic(bool) shutdown;
} task_queue_t;

// Worker thread statistics
typedef struct
{
  _Atomic(long) tasks_executed;
  _Atomic(long) total_execution_time_ns;
  _Atomic(long) idle_time_ns;
  struct timespec last_task_end;
  int cpu_affinity;
} worker_stats_t;

// Thread pool structure
typedef struct
{
  pthread_t *threads;
  worker_stats_t *worker_stats;
  int num_threads;
  task_queue_t task_queue;
  _Atomic(bool) shutdown;
  _Atomic(bool) immediate_shutdown;

  // Work stealing support
  task_queue_t *local_queues;
  _Atomic(int) *queue_locks;

  // Performance monitoring
  _Atomic(long) total_tasks_submitted;
  _Atomic(long) total_tasks_completed;
  struct timespec start_time;

  // Dynamic resizing
  pthread_mutex_t resize_mutex;
  int min_threads;
  int max_threads;
  _Atomic(int) active_threads;

  // Load balancing
  _Atomic(int) round_robin_index;
} thread_pool_t;

// Initialize task queue
int task_queue_init(task_queue_t *queue, int num_priorities);
// Add task to priority queue
int task_queue_push(task_queue_t *queue, task_t *task);
// Pop task from highest priority queue
task_t *task_queue_pop(task_queue_t *queue);
// Try to pop task without blocking
task_t *task_queue_try_pop(task_queue_t *queue);
// Worker stealing implementation
task_t *steal_task(thread_pool_t *pool, int worker_id);
// Worker thread function
void *worker_thread(void *arg);
// Create thread pool
thread_pool_t *thread_pool_create(int num_threads, int min_threads, int max_threads, bool enable_work_stealing, int num_priorities);
// Submit task to thread pool
int thread_pool_submit(thread_pool_t *pool, void (*function)(void *), void *argument, int priority);
// Set CPU affinity for worker thread
int thread_pool_set_affinity(thread_pool_t *pool, int worker_id, int cpu_id);
// Get thread pool statistics
void thread_pool_statistics(thread_pool_t *pool);
// Dynamic thraed pool resizing
int thread_pool_resize(thread_pool_t *pool, int new_size);
// Destroy thraed pool
void thread_pool_destroy(thread_pool_t *pool);

#endif