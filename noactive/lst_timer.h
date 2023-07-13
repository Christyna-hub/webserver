#ifndef LST_TIMER
#define LST_TIMER

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 64
class util_timer;       //   前向声明

// 用户数据结构
struct client_data
{
    sockaddr_in address;        // 客户端socket地址
    int sockfd;                 // socket文件描述符
    char buf[ BUFFER_SIZE ];    // 读缓存
    util_timer* timer;          // 定时器
};

// 定时器类
class util_timer {
public:
    util_timer() : prev( NULL ), next( NULL ) {}

public:
    time_t expire;          // 任务超时时间，这里使用绝对时间
    void (*cb_func)( client_data* );    // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    client_data* user_data; // 用户数据
    util_timer* prev;   // 指向前一个定时器
    util_timer* next;   // 指向后一个定时器
};

// 定时器链表，它是一个升序、双向链表，且带有头结点和尾结点。
class sort_timer_lst {
public:
    sort_timer_lst() : head( NULL ), tail( NULL ) {}
    // 链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst() {
        util_timer* tmp = head;
        while (!tmp) {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    // 将目标定时器timer添加到链表中
    void add_timer(util_timer* timer) {
        if ( !timer ) {
            // 定时器为空
            return;
        }

        if ( !head ) {
            // 链表上无节点
            head = tail = timer;
            return;
        }

        /*  如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，
        则把该定时器插入链表头部，作为链表新的头结点，
        否则就需要重载辅助函数add_timer()
        把它插入链表中合适的位置，以保证链表的升序特性*/
        if (timer->expire < head->expire) {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }

    /*
        当某个定时任务发生变化时，调整对应的定时器在链表中得位置，
        这个函数只考虑被调整的定时器的超时时间延长的情况，
        即该定时器需要往链表的尾部移动
    */
   void adjust_timer(util_timer* timer) {
        if ( !timer ) {
            return;
        }

        util_timer* tmp = timer->next;
        // 如果被调整的目标定时器处在链表的尾部
        // 或者该定时器的新的超时时间值仍然小于其下一个定时器的超时时间则不用调整。
        if ((timer->expire < tmp->expire) || !tmp) {
            return;
        }

        // 如果目标定时器是链表的头结点，则将该定时器取出，重新加入链表
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        } else {
            // 如果目标定时器不是链表的头节点，则将该定时器从链表中取出，再插入其中
            timer->next->prev = timer->prev;
            timer->prev->next = timer->next;
            add_timer( timer, timer->next );
        }
   }

   // 将目标定时器timer从链表中删除
   void del_timer( util_timer* timer) {
        if ( !timer ) {
            // 定时器是空的
            return;
        }

        // 定时器链表上只有一个定时器即timer
        if ( (timer == head) && (timer == tail)) {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }

        /* 如果链表中至少有两个定时器，且目标及是链表的头结点
        则将头节点置为源头节点的下一个节点，然后删除源节点
        */
       if (timer == head) {
            head = timer->next;
            timer->next = NULL;
            delete timer;
            return;
       }

       /*   至少有两个节点，并且目标定时器是尾结点，
       将尾结点置为原节点的上一个节点*/
       if (timer == tail) {
            tail = timer->prev;
            timer->prev = NULL;
            delete timer;
            return;
       }

        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
   }

   /*   SIGALARM 信号被触发就在其信号处理函数中执行一次 tick() 函数、
        以处理链表上到期任务*/
    void tick() {
        if ( !head ) {
            // 链表上没有节点处理
            return;
        }
        printf( "timer tick\n" );
        time_t cur = time(NULL); // 获取当前系统时间
        util_timer* tmp = head;
        // 从头开始处理每个定时器，直到遇到一个到期的定时器
        // 定时器是以过期时间升序排列
        while ( tmp ) {
            /*  因为每个定时器都使用绝对时间作为超时值，所以可以把
            定时器的超时值和系统当前时间进行比较，以此判断定时器是否到期*/
            if (cur < tmp->expire) {
                // 此后都是未超时的定时器
                break;
            }

            // 调用定时器的回调函数，以执行定时任务
            tmp->cb_func( tmp->user_data );
            // 执行完定时任务，则将他们从定时器中删除
            head = tmp->next;
            if (head) {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    /*  一个重载的辅助函数，它被共有的 add_timer 函数和 adjust_timer 函数调用
    该函数表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中*/
    void add_timer(util_timer* timer, util_timer* lst_head) {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        /* 遍历lst_head节点之后的所有节点，直到找到一个超时时间大于timer的节点
        将timer插入该节点之前*/
        while (tmp) {
            if (timer->expire < tmp->expire) {
                timer->next = tmp;
                timer->prev = prev;
                prev->next = timer;
                tmp->prev = timer;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }

        /*  如果遍历完 lst_head节点之后的部分链表，仍未找到超时时间大于目标定时器的
        超时时间的节点，则将目标定时器插入链表尾部，并把它设置为链表新的尾结点*/
        if (!timer) {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
        
    }


private:
    util_timer* head;   // 头结点
    util_timer* tail;   // 尾结点

};
#endif