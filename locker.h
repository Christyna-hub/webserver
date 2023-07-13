#ifndef LOCKER_H
#define LOCKER_H

// 线程同步机制实现
#include <pthread.h>
#include <exception>
#include <semaphore.h>

// 互斥量
class locker {
public:
    locker() {
        if(pthread_mutex_init(&m_mutex,NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t* get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 条件变量类 wait timedwait signal broadcast
class cond {
public:
    cond() {
        if(pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t * m_mutex) {
        // 等待条件被发出信号，阻塞调用线程直到另一个线程调用 signal() 或 broadcast() 发出信号。
        // 它接受一个指向互斥锁（pthread_mutex_t）的指针，
        // 在调用 wait() 前调用线程必须将其锁定。
        // 从 wait() 返回时，调用线程再次锁定互斥锁。返回值指示等待是否成功。
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }

    bool timedwait(pthread_mutex_t * mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }

    bool signal() {
        // 唤醒一个等待该条件变量等待线程（如果有）。返回值指示发出信号是否成功。
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        // 发出条件信号，唤醒所有等待线程。返回值指示发出信号是否成功。
        return pthread_cond_broadcast(&m_cond);
    }

private:
    pthread_cond_t m_cond;
};

// 信号量
class sem {
public:
    sem() {
        if(sem_init(&m_sem,0,0) != 0) {
            throw std::exception();
        }
    }
    
    sem(int num) {
        if(sem_init(&m_sem,0,num) != 0) {
            throw std::exception();
        }
    }

    ~sem() {
        sem_destroy(&m_sem);
    }

    // 等待信号量
    bool wait() {
        return sem_wait(&m_sem) == 0;
    }

    // 增加信号量
    bool post() {
        return sem_post(&m_sem) == 0;
    }
    
private:
    sem_t m_sem;
};
#endif