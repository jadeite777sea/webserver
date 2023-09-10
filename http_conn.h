#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include<sys/epoll.h>
#include<stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include<signal.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<sys/stat.h>
#include <sys/mman.h>
#include<errno.h>
#include<string.h>
#include "locker.h"
#include <sys/uio.h>



// HTTP请求方法，这里支持GET
enum METHOD{GET=0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT};

/*
解析客户端请求时，主状态机的状态
CHECK_STATE_REQUESTLINE:当前正在分析请求行
CHECK_STATE_HEADER:当前正在分析头部字段
CHECK_STATE_CONTENT:当前正在解析请求体
*/

enum CHECK_STATE {CHECK_STATE_REQUESTLINE=0,CHECK_STATE_HEADER,CHECK_STATE_CONTENT};

/*
服务器处理HTTP请求的可能结果，报文解析的结果
NO_REQUEST          :   请求不完整，需要继续读取客户数据
GET_REQUEST         :   表示获得了一个完成的客户请求
BAD_REQUEST         :   表示客户请求语法错误
NO_RESOURCE         :   表示服务器没有资源
FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
FILE_REQUEST        :   文件请求,获取文件成功
INTERNAL_ERROR      :   表示服务器内部错误
CLOSED_CONNECTION   :   表示客户端已经关闭连接了
*/
enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};

//从状态机的三种可能状态，即行的读取状态，分别表示
// 1. 读取到一个完整的行 2. 行出错 3. 行数据尚且不完整
enum LINE_STATUS{LINE_OK = 0, LINE_BAD, LINE_OPEN};

#define BUFFER_SIZE 64

class util_timer;       //向前声明定时器节点
class sort_timer_lst;   //向前声明双向链表
class http_conn;
//定时器类
class util_timer{
public:
    util_timer():prev(NULL),next(NULL){}

    time_t expire;                      //任务超时时间，这里使用绝对时间
    void  (*cb_func)(http_conn*);     //任务回调函数，回调函数处理的客户数量
    http_conn* user_data;
    util_timer* prev;                   //指向前一个定时器
    util_timer* next;                   //指向后一个定时器

};
//定时器链表，它是一个升序、双向链表，且带有头节点和尾节点
class sort_timer_lst{
public:
    sort_timer_lst():head(NULL),tail(NULL){}
    //链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst()
    {
        util_timer* tmp=head;
        while(tmp){
            head=tmp->next;
            delete tmp;
            tmp =head;
        }
    }
    void add_timer(util_timer* timer)
    {
        if(!timer)
        {
            return ;
        }
        if(!head)
        {
            head=tail=timer;
        }
        /*如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部，作为链表新的头节点
        否则就需要重新调用重载函数add_timer(),把它插入链表中合适的位置*/
        if(timer->expire < head->expire)
        {
            timer->next=head;
            head->prev=timer;
            head=timer;
            return ;
        }
        add_timer(timer,head);



    }
    //当某个定时任务发生变化时，调整对应定时器在链表中的位置。这个函数只考虑被调整的定时器的超时时间言正常的情况，即该定时器需要往链表的尾部移动
    void adjust_timer(util_timer* timer)
    {
        if(!timer)
        {
            return ;
        }
        util_timer*tmp =timer->next;
        //如果被调整的目标定时器处在链表的尾部，或者该定时器新的超时时间值仍然小于其下一个定时器的超时时间则不用调整
        if(!tmp||(timer->expire <tmp->expire))
        {
            return ;
        }
        //如果目标定时器是链表的头节点，则将该定时器从链表中去除并重新插入链表
        if(timer ==head)
        {
            head =head->next;
            head->prev =NULL;
            timer->next =NULL;
            add_timer(timer,head);
        }
        else{
            //如果目标定时器是链表的头节点，将该定时器从链表中取出，然后插入其原来所在位置后的部分链表中
            timer->prev->next =timer ->next;
            timer->next->prev =timer ->prev;
            add_timer(timer,timer->next);
        }

    }
    //将目标定时器timer从链表中删除
    void del_timer(util_timer* timer)
    {
        //双向链表没有节点
        if(!timer)
        {
            return ;
        }
        //下面这个条件成立表示链表中只有一个定时器，即目标定时器
        if((timer==head)&&(timer==tail))
        {
            delete timer;
            head=NULL;
            tail =NULL;
            return;
        }
        /*如果链表中至少有两个定时器，且目标定时器是链表的头结点，
        则将链表的头结点充值为原头结点的下一个节点，然后删除目标定时器*/
        if(timer == head)
        {
            head =head->next;
            head->prev =NULL;
            delete timer;
            return ;
        }
        //2个以上定时器，定时器为尾部节点
        if(timer==tail)
        {
            tail=tail->prev;
            delete tail->next;
            tail->next=NULL;
            return ;
        }
        //2个以上定时器，定时器为中间节点
        timer->prev->next=timer->next;
        timer->next->prev=timer->prev;
        delete timer;
        return ;

    }
    /*SIGALARM信号每次被触发就在其信号处理函数中执行一次tick()函数，以处理链表上
    到期任务。*/
    void tick()
    {
        if(!head)
        {
            return ;
        }
        printf("timer tick\n");
        time_t cur=time(NULL);
        util_timer *tmp=head;
        //从头结点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
        while(tmp)
        {
            if(cur<tmp->expire)
            {
                break;
            }
            //调用定时器的回调函数，以执行定时任务
            tmp->cb_func(tmp->user_data);
            //执行完定时任务就将它从链表中删除，并重置链表头结点
            head = tmp->next;
            if(head)
            {
                head->prev=NULL;
            }
            delete tmp;
            tmp =head;

        }
    }
private:
    void add_timer(util_timer* timer,util_timer* lst_head)
    {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        //遍历list_head节点之后的部分链表，知道找到一个超时时间大于目标节点，并将目标定时器插入该节点之前
        while(tmp)
        {
            if(timer->expire<tmp->expire)
            {
                prev->next =timer;
                timer->next=tmp;
                tmp->prev =timer;
                timer->prev=prev;
                break;
            }
            prev =tmp;
            tmp = tmp->next;
        }
        //如果遍历完lst_head 节点之后的部分链表，仍未找到超时时间大于目标节点，则将目标定时器插入链表尾部，并把它设置为链表新的尾部节点
        if(!tmp)
        {
            prev->next =timer;
            timer->prev =prev;
            timer ->next =NULL;
            tail =timer;
        }
        
    }
    util_timer*head;    //头节点
    util_timer*tail;    //尾节点



};



class http_conn{
public:

    static int m_epollfd; //所有socket上的时间都被注册到同一个epoll中
    static int m_user_count; //统计用户数量
    static const int WRITE_BUFFER_SIZE=1024;
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUFFER_SIZE=2048;
    util_timer* timer;                          //定时器
    int m_sockfd;                            //该 HTTP链接的socket

    

    http_conn(){}
    ~http_conn(){}
    void process(); //处理客户端的请求
    void init(int sockfd,const sockaddr_in & addr); //初始化新接收的连接
    void close_conn(); //关闭连接
    bool read(); //非阻塞的读
    bool write(); //非阻塞的写
    bool add_status_line(int status,const char* title);
    bool add_content_length(int content_len);
    bool add_content_type();
    bool add_blank_line();
    bool add_linger();
    bool add_headers(int content_len);

    
    


    



private:
    
    sockaddr_in m_address;                   // 通信的socket地址

    char m_read_buf[READ_BUFFER_SIZE];       //读缓冲区
    int m_read_idx;                          //标识度缓冲区以及读入客户端数据的最后一个字节的下一个位置
    char m_write_buf[WRITE_BUFFER_SIZE];     //写缓冲区
    int m_write_idx;                         //写缓冲区待发送的字节数
    int bytes_to_send;                       //将要发送字节
    int bytes_have_send;                     //已经发送字节
    struct iovec m_iv[2];                    //我们将采用writev来执行写操作，多以定义下面两个成员，其中m_iv_count表示被写内存块的数量
    int  m_iv_count;                         //其中m_iv_count表示被写内存块的数量
    HTTP_CODE process_read();                //解析HTTP请求
    HTTP_CODE parse_request_line(char*text); //解析请求首行
    HTTP_CODE parse_headers(char*text);      //解析请求头
    HTTP_CODE parse_content(char*text);      //解析请求体  
    LINE_STATUS parse_line();                //解析行 
    bool process_write(HTTP_CODE ret);       //将响应放到写缓冲区里面   

    int m_checked_index;                     //当前正在分析的字符在读缓冲区的位置
    int m_start_line;                        //当前正在解析的行的起始位置

    char*m_url;                             //请求目标文件的文件名
    char*m_version;                         //协议版本，只支持HTTP1.1
    char*m_host;                            //主机名
    bool m_linger;                          //HTTP请求是否要保持连接
    int m_content_length;                   // HTTP请求的消息总长度


    METHOD m_method;                            //请求方法
    CHECK_STATE m_check_state;                 //主状态机所处的状态


    char m_real_file[FILENAME_LEN];            //客户请求的目标文件的完整路径，其内容等于doc_root+m_url,dic_root为网站根目录
    struct stat m_file_stat;                   // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    char* m_file_address;                      // 客户请求的目标文件被mmap到内存中的起始位置
    void init();                               //初始化连接其余的信息


    char * get_line() { return m_read_buf+m_start_line;}         

    HTTP_CODE do_request();
    bool add_response(const char* format,...);
    bool add_content(const char* content);
    
void unmap();





};







#endif