#include "http_conn.h"

//  定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/wh/webserver/resources";

int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL); // 获取文件描述符的状态
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option); // 为文件描述符设置非阻塞状态
    return old_option;
}
// 1. 当有新连接到达的时候，将连接加入到epoll内核表中，对epoll内核表的增删改操作
void addfd( int epollfd, int fd, bool one_shot ) {
    // 为该客户请求创建一个epoll事件
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if ( one_shot ) {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    // 将新请求的文件描述符加入到epoll内核表中
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置非阻塞状态
    setnonblocking(fd);
}

// 从epoll中移出监听的文件描述符
void removefd( int epollfd, int fd) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 2. 为每个客户端初始化一个连接，读取客户请求数据
// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;


// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接，外部调用初始化套接字地址 
void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd; // 监听套接字？
    m_address = addr; // 其中有套接字的port和ip地址

    // 端口复用 存放选项值的缓冲区
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ));
    addfd( m_epollfd, sockfd, true);
    m_user_count++;
    // 对连接进行初始化
    init();
}




void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_linger = false;                       // 默认不保持连接，即非长链接

    m_method = GET;                         // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

}   


// 3. 解析请求
// 循环读取客户数据，知道无数据可读或者对方关闭连接
bool http_conn::read() {
    if ( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    while (true) {
        // 从m_read_buf + m_read_idx索引处开始保存数据，大小是READ_BUFFER_SIZE
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
            READ_BUFFER_SIZE - m_read_idx, 0 );
            if (bytes_read == -1) {
                if ( errno == EAGAIN || errno == EWOULDBLOCK ) {
                    // 没有数据
                    break;
                }
                return false;
            } else if (bytes_read == 0) { // 对方关闭连接，没有数据可读
                return false;
            }
            m_read_idx += bytes_read;
    }
    return true;
}



// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for( ; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            if( (m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN; // 没有读取到完整的一行，还需继续读取因此解析行状态时open
            } else if ( m_read_buf[ m_checked_idx + 1] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK; //解析完一行
            }
            return LINE_BAD;
        } else if( temp == '\n') {
            if ( (m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1] == '\r') ) {
                m_read_buf[ m_checked_idx - 1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK; 
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析请求行，获取行的请求方法，请求资源，版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个最先出现在text中
    if (! m_url) {
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0'; // 置为空字符，字符串结束
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk(m_url, " \t");
    if (! m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0'; // /index.html\0HTTP/1.1\0\0
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        // 只能处理http1.1版本的连接
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
   if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c (一个无符号字符) 的位置。
        m_url = strchr(m_url, '/');
   }
   if ( !m_url || m_url[0] != '/') {
        return BAD_REQUEST;
   }
   m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
   return NO_REQUEST; // 继续解析
}

// 解析HTTP头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    // 遇到空行表示解析完毕
    if ( text[0] == '\0' ) {
        // 如果http请求有消息头，则还需要读取m_content_length字节的消息体，
        // 状态机转换到 CHECK_STATE_CONTENT 状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到一个完整的HTTP请求
        return GET_REQUEST; // get请求不需要解析请求体
    } else if ( strncasecmp ( text, "Connection:", 11 ) == 0) {
        // strncasecmp 需要指定比较的字符个数 strcasecmp不需要指定
        // 处理 Connection 头部字段，Connetcion: keep-alive
        text += 11;
        text += strspn( text, " \t" ); // text指向下一个信息
        if ( strcasecmp( text, "keep-alive" ) == 0) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn( text, " \t");
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5) == 0) {
        text += 5;
        text += strspn( text, " \t");
        m_host = text;
    } else {
        printf( "oop! unkonw header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们不真正解析HTTP请求的消息体，只是判断它是否被完整的读入
http_conn::HTTP_CODE http_conn::parse_content( char* text) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx)) {
        // 若读缓冲区的读入字节的下一个位置 >= 正在分析的字符串在读缓冲区的位置+消息体长度，说明成功将消息体读入
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;  // 解析行状态
    HTTP_CODE ret = NO_REQUEST;         // 解析请求状态
    char* text = 0;
    // 一行一行地进行处理
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
        || ((line_status = parse_line()) == LINE_OK)) {
        // 开始解析 && 为解析完时继续解析
        text = get_line(); // 字符串数组，遇到'\0'则会自动结束
        m_start_line = m_checked_idx; // 更新行起止位置，checked主要用于解析
        printf("got 1 http line: %s\n", text );
        switch( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN; // 置为open下次继续解析
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }

    }
    return NO_REQUEST;

}



// 当得到一个完整、正确的HTTP请求时，我们需要分析目标文件的属性，
// 如果目标文件存在，对所有用户可读，并且不是目录，
// 则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    // "/webserver/resources"
    strcpy( m_real_file, doc_root ); // 将docroot复制到readfile中
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 ); // 把根和url拼接起来
    // 获取m_real_file文件的相关状态信息， -1失败， 0 成功
    if ( stat( m_real_file, &m_file_stat) < 0) {
        return NO_REQUEST;
    }
    // 怎么实现文件状态读取的？？有点神奇，m_real_file只是一个数组，怎么获取它的状态的，哪里设置的

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close(fd); // 打开文件完成映射后需要关闭文件描述符
    return FILE_REQUEST;
}

// 4.写响应数据

void http_conn::unmap() {
    if ( m_file_address ) {
        /*
            addr 是一个指向要释放的内存映射区域的指针，
            length 是该区域的长度。
            函数返回值为 0 表示释放成功，
            返回 -1 表示释放失败，失败时可以通过 errno 变量获取错误码。
        */
        munmap( m_file_address, m_file_stat.st_size ); // 释放由 mmap 函数分配的内存映射区域。
        m_file_address = 0;
    }
}

bool http_conn::write() {
    int temp = 0;
    int bytes_have_send = 0;             // 已经发送的字节
    int bytes_to_send= m_write_idx;     // 将要发送的字节，m_write_idx写缓冲区待发送的字节数

    if ( bytes_to_send == 0 ) {
        // 将要发送的字节数为0，说明相应结束
        modfd( m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true; // 初始化连接，此时连接断开了吗？应该是断开了
    }
    
    while (1)
    {
        // 分散写 将缓冲区的数据包一次发送
        temp = writev(m_sockfd, m_iv, m_iv_count ); // 返回已发送的字符数
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件
            // 在此期间服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp; // 待发送的字符数
        bytes_have_send += temp; // 已发送的字符数
        if (bytes_to_send <= bytes_have_send ) {
            // 发送http相应成功，根据HTTP请求中的Connetcion字段决定是否立即断开连接
            unmap();
            if (m_linger) {
                // 如果是长连接则初始化连接
                init();
                // 修改文件描述符
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            }
        }
    }
    
}

// 向写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    // ... 表示其后还可以有参数
    if ( m_write_idx >= WRITE_BUFFER_SIZE) {
        // 已经写满了,直接返回错误，写失败
        return false;
    }
    // va_list 声明接收可变参数列表的指针
    va_list arg_list; // arg_list为访问指针，初始化
    va_start( arg_list, format);
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list ); //将格式化的字符串输出到一个字符数组中
    if ( len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list ); //清理内存并关闭可变参数列表的访问。
    return true;
}

bool http_conn::add_status_line( int status, const char* title) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

void http_conn::add_headers( int content_len) {
    // 响应头 响应正文长度、正文类型（图片/二进制字符串），连接类型，空行
    add_content_length( content_len );
    add_content_type();
    add_linger();
    add_blank_line(); 
    // 这个函数里没有返回值么?
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-length: %d\r\n", content_len);
}

bool http_conn::add_content_type() {
    return add_response( "Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_linger() {
    return add_response( "Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line() {
    return add_response( "%s", "\r\n");
}

bool http_conn::add_content(const char* content) {
    return add_response( "%s", content );
}
// 返回数据

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch ( ret ) {
        case INTERNAL_ERROR:
            // 服务器内部错误返回500
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            } 
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line( 200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 线程池的工作线程执行程序，处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求,将数据读入，返回读后状态
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        // 若请求未被读取完,则继续读取
        modfd( m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
