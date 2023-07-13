#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include "lst_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 将给定的文件描述符设置为非阻塞模式
// 当没有数据可读或者可写的时候，函数立即返回
int setnonblocking( int fd ) 
{   
    int old_option = fcntl(fd, F_GETFL ); // 获取旧的模式
    int new_option = old_option | O_NONBLOCK; // 获取新模式
    fcntl( fd, F_SETFL, new_option ); // 设置为阻塞
    return old_option; // 将原有的模式返回
}

void addfd( int epollfd, int fd ) 
{
    epoll_event event; // 注册一个事件
    event.data.fd = fd; // 事件信息，携带的文件描述符
    event.events = EPOLLIN | EPOLLET; // 可读事件且边缘触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

void sig_handler( int sig ) 
{
    // 信号回调函数
    // 写入信号
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig( int sig ) {
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void timer_handler() {

    // 定时处理任务，实际上就是调用tick函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次 SIGALARM 信号， 所以需要我们重新定时，以不断触发SIGALARM信号
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( client_data* user_data) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert (user_data );
    close( user_data->sockfd );
    printf( " close fd %d\n", user_data->sockfd );
}



int main( int argc, char* argv[] ) {
    if (argc <= 1) {
        printf( "usage : %s port_number\n", basename( argv[0]));
        return 1;
    }
    int port = atoi( argv[1] );

    int ret = 0;
    // 客户端sock地址
    struct sockaddr_in address;
    // 初始化为0
    bzero(&address, sizeof(address));   // 将address结构体内存区域设置为0
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons( port );

    // 监听
    int listenfd = socket( PF_INET, SOCK_STREAM, 0);
    assert( listenfd >= 0);

    ret = bind( listenfd, (struct sockaddr*)& address, sizeof(address) );
    assert( ret != -1);

    ret = listen( listenfd, 5);
    assert( ret != -1);

    epoll_event events[ MAX_EVENT_NUMBER ];
    epollfd = epoll_create(5); // 创建epoll实例，最多可监听5个文件描述符
    assert( epollfd != -1 );
    addfd( epollfd, listenfd );

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1);
    setnonblocking( pipefd[1] ); // 管道写端，设置为非阻塞
    addfd( epollfd, pipefd[0] ); // 管道读端，放入到epoll中，监听，有可读的数据时触发，非阻塞

    // 设置信号处理函数
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT]; // 最大文件描述符
    bool timeout = false;
    alarm(TIMESLOT); // 定时，5秒后产生SIGALARM信号

    while ( !stop_server ) {
        // 等待监听事件的发生
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR)) {
            printf( "epoll failure\n");
            break;
        }

        for ( int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            if ( sockfd == listenfd ) {
                // 建立新的客户连接,建立新的定时器，将新定时器加入到链表
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                // 建立连接
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                addfd( epollfd, connfd ); // 加入到监听事件中
                users[connfd].address = client_address;
                users[connfd].sockfd = connfd;

                /*  创建定时器，设置其回调函数和超时时间，绑定定时器与用户数据
                最后将定时器添加到链表 timer_lst中 */ 
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT; // 超时时间是当前时间+15s
                users[connfd].timer = timer;
                // 将该连接加入到定时器中
                timer_lst.add_timer( timer );
            } else if ( ( sockfd == pipefd[0]) && ( events[i].events & EPOLLIN ) ) {
                // 管道中有可读事件，且当前events[i]上有可读事件
                // 处理信号
                int sig;
                char signals[1024];
                // 从管道中，读ret字节的数据到signals中
                ret = recv( pipefd[0], signals, sizeof( signals), 0 );
                if ( ret == -1 ) {
                    // 错误
                    continue;
                } else if ( ret == 0) {
                    // 啥也没读到
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i ) {
                        switch ( signals[i] ) 
                        {
                        case SIGALRM/* constant-expression */:
                        {    /* code */
                            // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                            // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
            else if ( events[i].events & EPOLLIN ) 
            {
                memset( users[sockfd].buf, '\0', BUFFER_SIZE ); // 初始化64B大小的空间
                ret = recv( sockfd, users[sockfd].buf, BUFFER_SIZE-1, 0 ); // 从sockfd中读取数据存放到users[sockfd]
                printf( "get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd );
                util_timer* timer = users[sockfd].timer;
                if ( ret < 0 ) 
                {
                    // 如果发生读错误，则关闭连接，并移除其对应的定时器
                    if ( errno != EAGAIN ) 
                    {
                        cb_func( &users[sockfd] );
                        if ( timer ) {
                            timer_lst.del_timer( timer );
                        }
                    } 
                }
                else if ( ret == 0 ) 
                {
                    // 如果对方已经关闭连接，那我们也关闭连接，并移出对应的定时器。
                    cb_func( &users[sockfd] );
                    if ( timer ) {
                        timer_lst.del_timer( timer );
                    }
                }
                else 
                {
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器
                    // 一延迟该链接被关闭的时间
                    if ( timer ) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                }
            }
        }

        // 最后处理定时事件，因为I/0事件有更高的优先级，当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    return 0;

} 