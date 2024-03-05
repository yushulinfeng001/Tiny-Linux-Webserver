#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();
    //初始化
    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();     //设置listenfd触发模式和connfd触发模式
    void sql_pool();        //初始化数据库连接池
    void log_write();       //初始化日志
    void trig_mode();       //初始化线程池

    void eventListen();     //监听端口，创建epollfd，设置信号处理函数
    void eventLoop();       //eventLoop循环，调用epoll_wait

    //初始化连接，初始化client_data数据，创建定时器并将定时器添加至utils的定时器链表上
    void timer(int connfd, struct sockaddr_in client_address);          //在接受客户端新连接是调用，传入connfd以及客户地址
    void adjust_timer(util_timer *timer);                               //更新定时器
    void deal_timer(util_timer *timer, int sockfd);                     //删除定时器，调用回调函数，关闭超时连接（关闭connfd,将connfd从epollfd移除），并将定时器从链表中移除
    bool dealclinetdata();                                              //处理客户端新连接
    bool dealwithsignal(bool& timeout, bool& stop_server);              //处理信号
    void dealwithread(int sockfd);                                      //处理读
    void dealwithwrite(int sockfd);                                     //处理写

public:
    //基础
    int m_port;                         //端口号
    char *m_root;                       //root文件夹路径
    int m_log_write;                    //异步日志或者同步日志
    int m_close_log;                    //是否关闭日志
    int m_actormodel;                   //actor模型    

    int m_pipefd[2];
    int m_epollfd;                      //epoll文件描述符
    http_conn *users;                   //连接用户

    //数据库相关
    connection_pool *m_connPool;        //数据库连接池    
    string m_user;                      //登陆数据库用户名
    string m_passWord;                  //登陆数据库密码
    string m_databaseName;              //使用数据库名
    int m_sql_num;                      //连接池数量

    //线程池相关
    threadpool<http_conn> *m_pool;      //http连接线程池
    int m_thread_num;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;                     //监听文件描述符
    int m_OPT_LINGER;                   //优雅关闭链接
    int m_TRIGMode;                     //触发模式
    int m_LISTENTrigmode;               //监听触发模式
    int m_CONNTrigmode;                 //连接触发模式

    //定时器相关
    client_data *users_timer;           //客户数据数组
    Utils utils;                        //工具，内含定时器排序链表，信号函数和fd操作函数
};
#endif
