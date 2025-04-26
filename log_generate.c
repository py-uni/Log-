/*
优化一 缓冲队列
    通过缓冲队列实现异步写入 减少I/O操作频率
    但需要生产者和消费者规定工作流程
    并且建议取消多log模式 否则会占据较多内存缓冲区
*/

/*
优化二 日志轮转
    按时间或文件大小分割日志文件 防止单个文件占据过大空间
    同样不建议使用多log模式 提高代码的维护难度
*/

/*
优化三 内存池
    预分配内存块 实现重复利用
    可以解决频繁使用 malloc/free 函数导致的内存碎片
*/

#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sys/queue.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

struct Err_info
{
    int error_type;
    char error_info[256];
};

// 任务结构体
struct task
{
    void (*taskfunc)(void *); // 任务执行函数
    void *arg;                // 任务参数
    STAILQ_ENTRY(task)
    entries; // 队列的下一成员指针
};

STAILQ_HEAD(task_queue, task);

// 线程池结构体
struct thread_pool
{
    pthread_mutex_t lock;         // 互斥锁
    pthread_cond_t cond;          // 条件变量
    pthread_t *threads;           // 线程数组
    struct task_queue task_queue; // 任务队列
    int thread_count;             // 线程数量
    int stop;                     // 线程池是否停止
};

// 线程池实例
struct thread_pool *pool = NULL;

// 工作线程函数
void *worker_thread(void *arg)
{
    pool = (struct thread_pool *)arg;
    while (1)
    {
        pthread_mutex_lock(&pool->lock);
        // 等待任务或关闭信号
        while (STAILQ_EMPTY(&pool->task_queue) && !pool->stop)
        {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        if (pool->stop)
        {
            pthread_mutex_unlock(&pool->lock);
            pthread_exit(NULL);
        }

        struct task *t = STAILQ_FIRST(&pool->task_queue);
        STAILQ_REMOVE_HEAD(&pool->task_queue, entries);
        pthread_mutex_unlock(&pool->lock);

        t->taskfunc(t->arg);
        free(t);
    }
}

// 提交任务到线程池
void thread_pool_submit(struct thread_pool *pool, void (*taskfunc)(void *), void *arg)
{
    struct task *t = malloc(sizeof(struct task));
    t->taskfunc = taskfunc;
    t->arg = arg;

    pthread_mutex_lock(&pool->lock);
    STAILQ_INSERT_TAIL(&pool->task_queue, t, entries);
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

// 创建线程池
struct thread_pool *thread_pool_create(int threads)
{
    pool = malloc(sizeof(struct thread_pool));
    pool->thread_count = threads;
    pool->threads = malloc(threads * sizeof(pthread_t));
    STAILQ_INIT(&pool->task_queue);
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->stop = 0;

    for (int i = 0; i < threads; i++)
    {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }
    return pool;
}

// 销毁线程池
void thread_pool_destroy(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    // 等待所有线程退出后释放资源
    for (int i = 0; i < pool->thread_count; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);
}

const char *get_log_filename(int error_type)
{
    switch (error_type)
    {
    case 0:
        return "error.log";
    case 1:
        return "info.log";
    case 2:
        return "run.log";
    case 3:
        return "inet.log";
    case 4:
        return "mem.log";
    }
}

pthread_mutex_t error_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t run_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t inet_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mem_mutex = PTHREAD_MUTEX_INITIALIZER;

void add_info(struct Err_info A)
{
    pthread_mutex_t *mutex = NULL;
    switch (A.error_type)
    {
    case 0:
        mutex = &error_mutex;
        break;
    case 1:
        mutex = &info_mutex;
        break;
    case 2:
        mutex = &run_mutex;
        break;
    case 3:
        mutex = &inet_mutex;
        break;
    case 4:
        mutex = &mem_mutex;
        break;
    }
    pthread_mutex_lock(mutex);

    const char *filename = get_log_filename(A.error_type);
    FILE *fp = fopen(filename, "a");

    // 以下为获取时间戳的函数功能块 实际中一般存在于主函数中
    time_t now_time;
    struct tm local;
    time(&now_time);
    localtime_r(&now_time, &local);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "[%Y-%m-%d %H:%M:%S]", &local);

    if (fp == NULL)
    {
        return;
    }

    fprintf(fp, "%s [PID:%d][TID:%d][%d] %s\n", time_buf, 10, 10, A.error_type, A.error_info);
    fclose(fp);
    pthread_mutex_unlock(mutex);
}

// 通用日志处理函数
void log_handler(void *arg)
{
    struct Err_info *A = (struct Err_info *)arg;
    add_info(*A); // 调用统一的写入逻辑
    free(A);
}

void log_gn(struct Err_info A)
{
    struct Err_info *arg = malloc(sizeof(struct Err_info));
    *arg = A;
    thread_pool_submit(pool, log_handler, arg);
}

void signal_func(int sig)
{
    thread_pool_destroy(pool);
    _exit(-1);
    return;
}

int main()
{
    /*
    主程序部分
        假设主程序出现一个ERROR 生成Err_info A
        实际运行过程中 A是由其他服务程序生成的
    */

    pool = thread_pool_create(10);
    signal(SIGINT, signal_func);

    struct Err_info A = {0, "E1"};
    while (1)
    { // 主循环
        // 模拟持续产生日志事件（实际场景可能从队列/网络读取）
        log_gn(A);
        sleep(1); // 控制日志生成频率
    }

    return 0;
}