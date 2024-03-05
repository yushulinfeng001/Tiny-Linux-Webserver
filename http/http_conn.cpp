#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//全局变量
locker m_lock;              
map<string, string> users;

//全局函数

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//静态变量
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::initmysql_result(connection_pool* connPool)
{
    //先从连接池中取一个连接
    MYSQL* mysql = NULL;
    //用RAII机制管理资源
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
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
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}


//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];               //temp：将要分析的字节
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)      //该行仍有内容，并未读完
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')     //出现换行符，说明该行读完
            {
                m_read_buf[m_checked_idx++] = '\0';     // \r、\n都改为结束符\0
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        //为什么判断了/r，又要判断/n？因为若最后一个位置出现的是/r，返回LINE_OPEN。此时又有新的数据读入，则当前位置可能是/n。
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')             //前一个字符是\r，则接收完整
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;           //未发现换行符，说明读取的行不完整
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据，一次性将数据读取完
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)        //读至没有数据可读
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
//解析完成后主状态机的状态变为CHECK_STATE_HEADER
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //strpbrk，按从前到后顺序找出最先含有搜索字符串（strCharSet）中任一字符的位置并返回位置指针(char*)，若找不到则返回空指针NULL
    m_url = strpbrk(text, " \t");               //请求该行中最先含有空格和\t任一字符的位置并返回
    if (!m_url)                                 //没有目标字符，则代表报文格式有问题
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';                            //将前面的数据取出，后移找到请求资源的第一个字符
    char *method = text;
    if (strcasecmp(method, "GET") == 0)             //确定请求方式
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST; 
    //strspn，从str的第一个元素开始往后数，看str中是不是连续往后每个字符都在group中可以找到。到第一个不在gruop的元素为止
    //去除前面的空格和/t
    m_url += strspn(m_url, " \t");                       //得到url地址
    m_version = strpbrk(m_url, " \t");          
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");              //得到http版本号
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;                             //只接受HTTP/1.1版本
    
    //去除前面的http://
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //strchr，该函数返回在字符串 str 中第一次出现字符 c 的位置，如果未找到该字符则返回 NULL
        m_url = strchr(m_url, '/');
    }
    //去除前面的https://
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')                                //判断是空头还是请求头，空头需要改变状态机状态
    {
        if (m_content_length != 0)                      //具体判断是get请求还是post请求
        {
            m_check_state = CHECK_STATE_CONTENT;        //post请求需要改变主状态机的状态
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)         //解析头部连接字段
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)                //判断是否为长连接
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)     //解析请求头的内容长度字段
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);                          //atol(const char*str)：将str所指的字符串转换为一个long int的长整数
    }
    else if (strncasecmp(text, "Host:", 5) == 0)                //解析请求头部host字段
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;        //用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


//通过while循环，封装主状态机，对每一行进行循环处理
//此时，从状态机已经修改完毕，主状态机可以取出完整的行进行解析
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;          //初始化从状态机的状态
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();              // 获取一行的字符，从状态机已经将每一行末尾的“\r”、“\n”符号改为“\0”。
        m_start_line = m_checked_idx;   // 更新下一行的起始位置
        LOG_INFO("%s", text);
        switch (m_check_state)                      //三种状态转换逻辑
        {
            case CHECK_STATE_REQUESTLINE:           //正在分析请求行
            {
                ret = parse_request_line(text);     //解析请求行
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:                //正在分析头部字段       
            {
                ret = parse_headers(text);          //解析请求头
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)        //get请求，需要跳转到报文响应函数
                {
                    return do_request();            //响应客户请求
                }
                break;
            }
            case CHECK_STATE_CONTENT:               //解析消息体
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)             //post请求，跳转到报文响应函数
                    return do_request();
                line_status = LINE_OPEN;            //更新，跳出循环，代表解析完了消息体
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    //doc_root初始化时设定
    strcpy(m_real_file, doc_root);                      //将初始化的m_real_file赋值为网站根目录
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');                //找到m_url中“/”的位置

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        //strcat函数是字符串追加函数，也就是在字符串后面追加另一个字符串
        strcat(m_url_real, m_url + 2);
        //用于将一个字符串复制到另一个字符串，但strncpy函数不同与strcpy函数的是，strncpy可以指定复制的字符数量
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123  &  passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())                //说明库中没有重名
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);                   //在数据库中插入数据
                users.insert(pair<string, string>(name, password));         //操作user，共享资源，加锁
                m_lock.unlock();

                if (!res)                               //注册成功
                    strcpy(m_url, "/log.html");         
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }



    if (*(p + 1) == '0')                            //如果请求资源为/0，表示跳转注册界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')                       //如果请求资源为/1，表示跳转登录页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));     //将网站目录和/log.html进行拼接，更新到m_real_file中

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)     //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体;失败返回NO_RESOURCE状态，表示资源不存在
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))        //判断文件类型，客户端是否有访问权限
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))            //判断该路径是否为目录
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);        //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    //内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);                                   //避免文件描述符的浪费和占用
    return FILE_REQUEST;                         //表示请求文件存在且可以访问
}


void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)                                 //要发送的数据长度为0，表示响应报文为空，一般不会出现该情况
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        //writev用于在一次函数调用中写多个非连续缓冲区，有时将该函数称为聚集写
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)                                        //判断缓冲区是否已满
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);       //重新注册写事件，等待下一次触发
                return true;
            }
            unmap();                                                    //发送失败，但不是缓冲区问题，取消映射
            return false;
        }

        bytes_have_send += temp;                                        //更新已发送字节数
        bytes_to_send -= temp;
        //调整iovec中的指针和长度
        if (bytes_have_send >= m_iv[0].iov_len)                         //第一个iovec信息的数据已发送完，发送第二个iovec数据
        {
            m_iv[0].iov_len = 0;                                        //不再继续发送第一个iovec信息
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else			                                                //继续发送第一个iovec信息的数据
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)                                             //判断条件，数据已全部发送完
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);                //在epoll树上重置EPOLLONESHOT事件

            if (m_linger)                                                   //浏览器的请求为长连接
            {
                init();                                                     //重新初始化HTTP对象
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:                                //内部错误，500
    {
        add_status_line(500, error_500_title);          //状态行
        add_headers(strlen(error_500_form));            //消息报头
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:                                   //报文语法有误，404
    {
        add_status_line(404, error_404_title);          
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:                             //资源没有访问权限，403
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:                                  //文件存在，200
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)                   //请求的资源存在
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;             //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;          //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_len = m_file_stat.st_size;      
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;      //发送的全部数据为响应报文头部信息和文件大小
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";   //请求的资源大小为0，则返回空白html文件
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;                   //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    // 解析请求报文
    HTTP_CODE read_ret = process_read();
    
    // 表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    //重新注册epollin事件，服务器主线程检测读事件，并重置oneshot事件
        return;
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);       //注册epollout事件，服务器主线程检测写事件，并重置oneshot事件
}



