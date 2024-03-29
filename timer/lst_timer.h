#ifndef LST_TIMER
#define LST_TIMER

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

#include <time.h>
#include "../log/log.h"

//前置声明
class util_timer;

//客户数据
struct client_data
{
    sockaddr_in address;    //客户地址
    int sockfd;             //连接文件描述符
    util_timer *timer;      //定时器指针
};

//定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                      //定时器的结束时间
    
    void (* cb_func)(client_data *);    //函数指针，将客户文件描述符从epoll实例中删除并关闭文件描述符
    client_data *user_data;             //客户数据
    util_timer *prev;                   //前一定时器
    util_timer *next;                   //后一定时器
};

//定时器排序链表
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();                      //将链表中所有节点指针删除

    void add_timer(util_timer *timer);      //添加定时器
    void adjust_timer(util_timer *timer);   //调整定时器
    void del_timer(util_timer *timer);      //删除定时器
    void tick();                            //将过了时间的定时器删除，调用定时器的回调函数指针              

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;       //头结点
    util_timer *tail;       //尾结点
};

//工具类
class Utils
{
public:
    Utils() {}          //构造函数
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);   

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;             //定时时间
};

//全局函数
void cb_func(client_data *user_data);

#endif
