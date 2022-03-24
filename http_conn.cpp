#include "http_conn.h"

using namespace std;
// 定义 http 响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
const char* doc_root = "/var/www/html";

int setnonblocking(int fd){
    int flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
    return flags;
}

void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int flag){
    epoll_event event;
    event.data.fd = fd;
    event.events = flag | EPOLLIN | EPOLLET | EPOLLRDHUP;
 
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close){
    if(real_close && (m_epollfd != -1)){
        removefd(m_epollfd, m_sockfd);
        m_sockfd -= 1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    //only debug
    //int reuse = 1;
    //setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;
    // HTTP初始时未要求保持连接
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;

    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于解析出一行内容
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;

    for(;m_checked_idx < m_read_idx; ++m_checked_idx){
        // 获得当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        // 如果当前字节是"\r"，即回车符，则说明可能读取到一个完整的行
        if(temp == '\r'){
            if(m_checked_idx + 1 == m_read_idx){
                // 没有读取到一个完整行，需要继续读取来作进一步分析
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx+1] == '\n'){
                // 读取到完整行了
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;

            }
            else{
                return LINE_BAD;
            }
        }
        // 如果当前字符是"\n"字符，也可能读取到一个完整行了
        else if(temp == '\n'){
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        
    }
    // 如果所有内容分析完毕也没有"\r"字符，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析
    return LINE_OPEN;
}

// 无阻塞循环读取用户数据直到无数据可读或者对方关闭连接
bool http_conn::read(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    while(true){
        ssize_t len = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(len == -1){
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return false;
        }
        // 没有数据可读
        else if(len == 0){
            return false;
        }
        m_read_idx += len;
    }
    return true;
}




// 解析请求行，获得请求方法、目标URL、以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    // 如果请求里没有空格和'\t'字符，这个请求肯定有问题
    m_url = strpbrk(text, " \t");
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char* method = text;
    // 暂时只考虑GET请求
    if(strcasecmp(method, "GET") == 0){
        m_method = GET;
    }
    else if(strcasecmp(method, "HEAD") == 0){
        m_method = HEAD;
    }
    else{
        return BAD_REQUEST;
    }

    m_url += strspn(m_url, " \t");

    // 判断m_version是否有问题
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }
    // 检查URL是否合法
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/'); //The  strchr() function returns a pointer to the first occurrence of the
                                    // character c('/') in the string s.
    }

    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }
    // HTTP请求行处理完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求的头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    // 遇到空行，表示头部字段解析完毕
    if(text[0] == '\0'){
        // 如果 HTTP 请求有消息体，还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT的状态
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    // 处理Connection头部字段
    else if(strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        text += strspn(text, " \t"); // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
        if(strcasecmp(text, "keep-alive") == 0){
            m_linger = true;
        }
    }

    // 处理Connection-Length头部字段
    else if(strncasecmp(text, "Content-Length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }

    // 处理Host头部字段
    else if(strncasecmp(text, "Host:", 5) == 0){
        text += 5; // move to the behind of "Host:"
        text += strspn(text, " \t");
        m_host = text;
    }
    else{
        printf("unknown headers %s\n", text);
    }
    return NO_REQUEST;

}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整地读入了 
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机 
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK; // 记录当前行的读取状态 
    HTTP_CODE ret = NO_REQUEST;        // 记录HTTP请求的处理结果
    char* text = 0;
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
    || (line_status = parse_line()) == LINE_OK){
        text = get_line();
        m_start_line = m_checked_idx;
        printf("got 1 http line: %s\n", text);

        switch (m_check_state)
        {
        // 分析请求行
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            break;
        }
            
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if(ret == GET_REQUEST){
                return do_request();
            }
            break;
        }
            
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            break;
        }
            
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性；如果目标文件存在、对所有用户可读，
// 并且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file, doc_root); // 先把根目录复制过来 
    int len = strlen(doc_root);
    strncpy(m_real_file+len, m_url, FILENAME_LEN - len - 1); // 把剩余路径复制过来
    if(stat(m_real_file, &m_file_stat) < 0){ // 把文件属性放到m_file_stat处
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_IROTH)){ // 判断文件是否可读
        return FORBIDDEN_REQUEST;
    }

    if(S_ISDIR(m_file_stat.st_mode)){ // 文件是目录
        return BAD_REQUEST;
    }
    if(m_method == HEAD) return FILE_REQUEST;
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write(){
    int temp = 0;
    int byte_have_send = 0;
    int byte_to_send = m_write_idx;
    while(true){
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1){
            // 如果是因为TCP写缓冲没有空间，则等待下一轮的EPOLLOUT事件，虽然在此期间， 
            // 服务器无法立即接收同一客户的下一个请求，但这可以保证连接的完整性
            if(errno == EAGAIN){
                // EPOLLOUT -- The associated file is available for write(2) operations
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
            }
            unmap();
            return false;
        }
        byte_to_send -= temp;
        byte_have_send += temp;
        if(byte_to_send <= byte_have_send){
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接 
            unmap();
            if(m_linger){
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else{
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }

    }
}

// 向写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ){
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    va_list arg_list; // arg_list 是指向参数的指针 
    va_start (arg_list, format); // 初始化刚定义的va_list变量，使其指向第一个可变参数的地址
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE-1-m_write_idx,
                        format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){
        return false;
    }
    m_write_idx += len;
    va_end(arg_list); // 停止参数的获取 
    return true;

}

bool http_conn::add_content( const char* content ){
    return add_response( "%s", content);
}
bool http_conn::add_status_line( int status, const char* title ){
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers( int content_len ){
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length( int content_length ){
    return add_response( "Content-Length: %d\r\n", content_length);
}
bool http_conn::add_linger(){
    return add_response( "Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line(){
    return add_response( "%s", "\r\n");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write( HTTP_CODE ret ){
    switch (ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title); // 服务器端执行请求发生错误，比较笼统
        add_headers(strlen(error_500_form));
        if(!add_content(error_500_form)){
            return false;
        }
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title); // 客户端请求语法错误 
        add_headers(strlen(error_400_form));
        if(!add_content(error_400_form)){
            return false;
        }
        break;
    case NO_RESOURCE:
        add_status_line(404, error_404_title); // 服务器上无法找到请求的资源
        add_headers(strlen(error_404_form));
        if(!add_content(error_404_form)){
            return false;
        }
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_title); // 服务器端拒绝访问
        add_headers(strlen(error_403_form));
        if(!add_content(error_403_form)){
            return false;
        }
        break;
    
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        if(m_method == HEAD) return true;
        if(m_file_stat.st_size != 0){
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        // 文件里为空
        else{
            const char* ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if(!add_content(ok_string)){
                return false;
            }
        }
        break;
    default:
        return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 等待下一轮EPOLLIN(read)事件
        return;
    }

    bool write_ret = process_write(read_ret);
    if(!write_ret){
        modfd(m_epollfd, m_sockfd, EPOLLOUT); // 等待下一轮EPOLLOUT(write)事件
        close_conn();
    }

    modfd(m_epollfd, m_sockfd, EPOLLOUT);

}


