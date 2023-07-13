#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536  //最大文件描述符的个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大事件数量

// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot);
extern void removefd( int epollfd, int fd);
// 移出文件描述符
// 添加信号捕捉，做信号处理
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa; // 
    // 清空,sa中的数据都为0
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1);
    // 注册哪个信号，信号参数
}
// 在命令行中需要输入参数，因此main函数中设置argc、argv
int main(int argc, char * argv[]) {

    if (argc <= 1) {
        // 至少传递一个端口号
        // basename()获取基础的名字，程序名称
        printf("按照如下格式运行： %s port_number\n",basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);
    addsig( SIGPIPE, SIG_IGN );

    threadpool< http_conn >* pool = NULL;
    try {
        //printf("console:\n");
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    // 申请一个http连接池，存储到达的所有连接
    http_conn* users = new http_conn[ MAX_FD ];

    // 创建监听文件描述符 被动套接字，由内核接收连接请求
    int listenfd = socket( PF_INET, SOCK_STREAM, 0);

    // 创建监听套接字
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    // 需要强制类型转换，sockaddr_in转换为sockaddr，绑定端口和ip
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) ); 
    ret = listen( listenfd, 5); // 开始监听，设置半连接+全连接的最大连接数量,若是请求的连接大于队列最大数量，则会对这些连接返回RST
    
    // listen中TCP为此维护两个队列
    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER]; // epoll连接池
    int epollfd = epoll_create( 5 );
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while (true) {

        // 返回epollfd内核时间表中触发的事件数量，触发事件保存在events中，-1表示阻塞时间没有限制
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );

        if ( ( number < 0) && ( errno != EINTR) ) {
            printf( "epoll failure\n" );
            break;
        }

        for (int i = 0; i < number; i++) {

            // 获得每一个连接的fd
            int sockfd = events[i].data.fd;
            
            if (  sockfd == listenfd ) {
                // 若触发的文件描述符是监听描述符则说明有新的连接到达
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                // 监听描述符上有新的连接到达，新建客户端的文件描述符
                int connfd = accept( listenfd, ( struct sockaddr* )& client_address, &client_addrlength);

                if ( connfd < 0 ) {
                    // -1则失败
                    printf( "errno is : %d\n", errno );
                    continue;
                }

                if ( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd); // 若当前连接数量 > 最大连接数则关闭连接
                }   
                users[connfd].init( connfd, client_address);
            
            }  else if ( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR) ) {
                
                users[sockfd].close_conn();

            } else if (events[i].events & EPOLLIN) {
                
                if (users[sockfd].read()) {
                    // 通知读取sockfd上的数据
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }
            } else if ( events[i].events & EPOLLOUT ) {
                // 有数据可向连接写
                if ( !users[sockfd].write() ) {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}