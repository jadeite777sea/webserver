#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include<signal.h>
#include "http_conn.h"
#include<assert.h>
#define MAX_FD 65535 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 //监听的最大事件数
#define TIMESLOT 2 //间隔时间


static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//超时信号处理函数
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

//添加信号捕捉
void addsig(int sig,void(handler)(int)){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig,&sa,NULL);

}
void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( http_conn* user_data )
{

    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->m_sockfd, 0 );
    
    if(user_data->m_sockfd!=-1)
    {
         close( user_data->m_sockfd );
         printf( "close fd %d\n", user_data->m_sockfd );
         user_data->m_sockfd=-1;
        user_data->m_user_count--; 
        printf("当前用户数量:%d\n",user_data->m_user_count);
    }
       
    
    
    
}

//添加文件描述符到epoll中
extern int addfd(int epollfd,int fd,bool one_shot);

//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd);

//修改文件描述符
extern void modfd(int epollfd ,int fd,int ev);


extern void setnonblocking(int fd);


int main(int argc,char*argv[])
{
    if(argc<=1)
    {
        printf("按照如下格式运行：%s port_number\n",basename(argv[0]));
        exit(-1);

    }

    //获取端口号
    int port=atoi(argv[1]);

    
    //对SIGPIE信号进行处理
    //当向一个已关闭的socket写入数据时，一般情况下操作系统会发送'SIGPIPE'信号给进程，如果进程没有处理这个信号，就会导致进程终止。SIG_IGN为一个宏表示忽略信号，当向已关闭的socket写入数据时，不做任何处理。
    addsig(SIGPIPE,SIG_IGN);
    //创建线程池，初始化线程池
    threadpool<http_conn>* pool =NULL;
    try{
        pool =new threadpool<http_conn>();
        printf("pool得到了初始化");

    }catch(...)
    {
        printf("bad");
        exit(-1);
    }
    printf("线程池创建成功");
    //创建一个数组保存所有的客户端信息
    http_conn *users =new http_conn[MAX_FD];
    //创建监听的套接字
    int listenfd =socket(PF_INET,SOCK_STREAM,0);
    //设置端口复用
    int reuse =1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    //绑定
    struct sockaddr_in address;
    address.sin_family =AF_INET;
    address.sin_addr.s_addr =INADDR_ANY;
    address.sin_port=htons(port);
    bind(listenfd,(struct sockaddr*)&address,sizeof(address));

    //监听
    listen(listenfd,5);

    //创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(5);


    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd,listenfd,false);
    http_conn:: m_epollfd=epollfd;
    
     // 创建管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0],false);


    // 设置信号处理函数
    addsig( SIGALRM ,sig_handler);
    addsig( SIGTERM ,sig_handler);
    bool stop_server = false;


   
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号


    while(!stop_server){
        int num=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((num<0)&& (errno!=EINTR))
        {
           
            break;
        }
        //循环遍历events数组
        for(int i=0;i<num;i++)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);
                if(http_conn:: m_user_count>= MAX_FD){
                    //目前连接数满了
                    // 给客户端写一个信息，服务器正忙
                    close(connfd);
                    continue;
                }
                // 将新的客户数据初始化, 放到数组中
                users[connfd].init(connfd,client_address);
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );

            }
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
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
            else if(events[i].events & (EPOLLRDHUP| EPOLLHUP| EPOLLERR))
            {                
                //对方异常断开或者错误等事件
                users[sockfd].close_conn();
            }
            else if(events[i].events&EPOLLIN)
            {

                bool ret=users[sockfd].read();
                util_timer* timer = users[sockfd].timer;
                if(ret)
                {
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        
                        timer_lst.adjust_timer( timer );
                    }
                    //一次把所有数据都读完
                    pool->append(users+sockfd);

                }
                else
                {
                    
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                    //本质上就是close_conn的操作
                    cb_func( &users[sockfd] );
                    
                }

            }
            else if(events[i].events& EPOLLOUT)
            {
                util_timer* timer = users[sockfd].timer;
                //一次性写完所有数据,如果不是长连接则直接关闭
                if(!users[sockfd].write())
                {   
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                    users[sockfd].close_conn();

                }
                

            }
        }
    
         // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;
}