#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    
    //请求方法
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    //主状态机的三种状态
    //主状态机的状态表明当前正在处理请求报文的哪一部分
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,        //当前正在分析请求行
        CHECK_STATE_HEADER,                 //当前正在分析头部字段
        CHECK_STATE_CONTENT                 //当前正在解析消息体（目前仅用于解析POST请求）
    };

    //从状态机的三种状态
    enum LINE_STATUS
    {
        LINE_OK = 0,        //完整读取一行
        LINE_BAD,           //报文语法有误
        LINE_OPEN           //读取的行不完整
    };

    //服务器处理HTTP请求的可能结果，报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };



public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    //关闭连接
    void close_conn(bool real_close = true);
    //完成请求报文的解析及响应
    void process();
    //向m_read_buf中读入请求报文
    bool read_once();
    //将内存映射区以及缓冲区中的数据发送给客户端
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);

    //reactor模式：只有reactor模式下，标志位improv和timer_flag才会发挥作用        
    int timer_flag;             //timer_flag：当http的读写失败后置1，用于判断用户连接是否异常
    int improv;                 //imporv：在read_once和write成功后会置1，对应request完成后置0，用于判断上一个请求是否已处理完毕


private:
    //初始化连接
    void init();

    //解析请求
    HTTP_CODE process_read();
    //向m_write_buf中写入响应报文
    bool process_write(HTTP_CODE ret);

    // 下面这一组函数被process_read调用以分析HTTP请求

    //解析请求行
    HTTP_CODE parse_request_line(char *text);
    ////解析请求头
    HTTP_CODE parse_headers(char *text);
    ////解析消息体
    HTTP_CODE parse_content(char *text);
    //对客户请求进行响应
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    //分析出一行内容,返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
    LINE_STATUS parse_line();

    // 这一组函数被process_write调用以填充HTTP应答

    void unmap();
    //添加响应
    bool add_response(const char *format, ...);
    //添加文本content
    bool add_content(const char *content);
    //添加状态行
    bool add_status_line(int status, const char *title);
    //添加消息报头
    bool add_headers(int content_length);
    //添加文本类型
    bool add_content_type();
    //添加Content-Length
    bool add_content_length(int content_length);
    //添加连接状态
    bool add_linger();
    //添加空行
    bool add_blank_line();

public:
    static int m_epollfd;           // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;        // 统计用户的数量
    MYSQL *mysql;       //数据库连接
    int m_state;        //读为0, 写为1

private:
    
    int m_sockfd;               // 该HTTP连接的socket
    sockaddr_in m_address;      // 对方的socket地址

    char m_read_buf[READ_BUFFER_SIZE];      // 读缓冲区
    long m_read_idx;                        // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    long m_checked_idx;                     // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                       // 当前正在解析的行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE];    // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数

    CHECK_STATE m_check_state;      //正在处理请求报文的哪一部分
    METHOD m_method;                //请求方法

    char m_real_file[FILENAME_LEN]; // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char *m_url;            // 客户请求的目标文件的文件名
    char *m_version;        // HTTP协议版本号，我们仅支持HTTP1.1
    char *m_host;           // 主机名
    long m_content_length;  // HTTP请求的消息总长度
    bool m_linger;          // HTTP请求是否要求保持连接
    char *m_file_address;   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;// 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];   // iovec定义向量元素，通常该结构用作一个多元素的数组，一个元素用来存响应数据，一个用来存响应http
    int m_iv_count;         // m_iv_count表示被写内存块的数量。
    int cgi;                // 是否启用的POST
    char *m_string;         // 存储请求头数据
    int bytes_to_send;      // 将要发送的数据的字节数
    int bytes_have_send;    // 已经发送的字节数
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
