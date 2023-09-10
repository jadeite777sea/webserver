#include "./http_conn.h"



// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

int http_conn::m_epollfd=-1; //所有socket上的时间都被注册到同一个epoll中
int http_conn::m_user_count=0; //统计用户数量

// 网站的根目录
const char* doc_root = "/home/zhangyan/webserver/resources";


void setnonblocking(int fd){
    int old_flag=fcntl(fd,F_GETFL);
    int new_flag= old_flag| O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
   
}


//向epoll中添加需要监听的文件描述符
void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd=fd;
    //event.events =EPOLLIN|EPOLLRDHUP|EPOLLET;
    event.events =EPOLLIN|EPOLLRDHUP|EPOLLET;
    
    if(one_shot)
    {
       event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    //设置文件描述符非阻塞
    setnonblocking(fd);

}

//从epoll中删除文件描述符
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//从epoll中移除监听的文件描述符，充值socket上EPOLLONESHOT时间，以确保下一次可读时
void modfd(int epollfd ,int fd,int ev)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);

}

void http_conn::init(int sockfd,const sockaddr_in&addr)
{
    m_sockfd=sockfd;
    m_address=addr;
    //端口复用
    int reuse =1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //添加到epoll对象中
    addfd(m_epollfd,sockfd,false);
    
    m_user_count++; //总用户数+1
    printf("总用户数%d\n",m_user_count);
    init();
}

//关闭连接·
void http_conn::close_conn(){

    //将定时器进行删除操作
    if(timer)
    {
        //timer_lst.del_timer( timer );
    }
    if(m_sockfd!=-1){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--; 
    }
}





//循环读取客户数据，直到无数据可读
bool http_conn::read()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;
    }
    //读取到的字节
    int bytes_read =0;
    while(true){
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1)
        {
            if(errno==EAGAIN||errno== EWOULDBLOCK)
            {
                //没有数据
                break;
            }
            return false;
        }
        else if(bytes_read==0)
        {
            //对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    //printf("读取到了数据: %s\n",m_read_buf);
    return true;

}

void http_conn::init()
{
    m_check_state=CHECK_STATE_REQUESTLINE;  //初始化状态为解析请求首行
    m_checked_index=0;
    m_start_line=0;
    m_read_idx =0;
    m_content_length=0;
    m_method =GET;
    m_url =0;
    m_linger=false;
    m_host=0;
    m_write_idx = 0;
    m_version =0;

    bzero(m_read_buf,READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
    bytes_have_send = 0;            
    bytes_to_send   = 0;


                          
}


HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char* text=0;

    //解析到了一行完整数据，或者解析到了请求体，也是完整的数据
    while(  ((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))||(line_status=parse_line())==LINE_OK)
    {
        //获取一行数据  
        text=get_line();
        m_start_line=m_checked_index;
       // printf("got 1 http line : %s\n",text);
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret=parse_request_line(text);
                if(ret ==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;

            }
            case CHECK_STATE_CONTENT:
            {
                ret=parse_content(text);
                if(ret == GET_REQUEST)
                {
                    //将请求内容保存准备进行下一步操作
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;


            }
            case CHECK_STATE_HEADER:
            {
               // printf("解析请求头跳转\n");
                ret=parse_headers(text);
                //printf("解析请求头返回值,%d\n",ret);
                if (ret ==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;

            }
            default:
            {
                return INTERNAL_ERROR;

            }

        }
       





    }
    return NO_REQUEST;

}
//解析HTTP请求行,获得请求方法,目标URL，HTTP版本
//正则表达式？
HTTP_CODE http_conn::parse_request_line(char*text)
{
    // GET /index.html HTTP/1.1
    m_url =strpbrk(text," \t");
    *m_url++='\0';
    char* method=text;
    // GET\0/index.html HTTP/1.1
    if(strcasecmp(method,"GET")==0)
    {
        m_method=GET;
    }
    else{
        return BAD_REQUEST;
    }
// /index.html HTTP/1.1
    m_version=strpbrk(m_url," \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++='\0';
    // /index.html\0HTTP/1.1
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
    // http://192.168.1.1:10000/index.html
    //加上7的原因是分片，一个完整的字符串如果不加字符数量，默认是读到末尾'\0'
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7; //192.168.1.1:10000/index.html
        m_url=strchr(m_url,'/');// /index.html
    }

    if(!m_url||m_url[0]!='/')
    {
        return BAD_REQUEST;
    }

    m_check_state=CHECK_STATE_HEADER; //主状态机检查状态变成检查请求头
   // printf("将状态转换为检查请求头\n");
    return NO_REQUEST;
}
/*  GET请求报文格式
GET /signin?next=%2F HTTP/2\r\n
Host: www.zhihu.com\r\n
User-Agent: Mozilla/5.0\r\n
Accept: \*\/*\r\n
Accept-Language: zh-CN\r\n
Accept-Encoding: gzip, deflate\r\n
Connection: keep-alive\r\n
Upgrade-Insecure-Requests: 1\r\n
Cache-Control: max-age=0\r\n
TE: trailers\r\n
\r\n
*/
/* POST请求报文格式
POST /login HTTP/1.1\r\n
Host: 127.0.0.1:8888\r\n
User-Agent: Mozilla/5.0\r\n
Accept: \*\/*\r\n
Accept-Language: zh-CN\r\n
Accept-Encoding: gzip, deflate\r\n
Content-Type: application/x-www-form-urlencoded\r\n
Content-Length: 29\r\n
Connection: keep-alive\r\n
\r\n
username=root&password=123456
*/
HTTP_CODE http_conn::parse_headers(char*text)
{
    //printf("解析请求头部中\n");
    //遇到空行,表示头部字段解析完成
    if(text[0]=='\0')
    {
        //状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length!=0)
        {
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
           // printf("内容状态转换成功\n");
        }
        else
        {
            //printf("内容为0\n");
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
        
    }
    else if(strncasecmp(text,"Connection:",11)==0)
    {
        // 处理Connection 头部字段  Connection: keep-alive
        text+=11;
        text+=strspn(text," \t");
        if ( strcasecmp( text, "keep-alive" ) == 0 ) 
        {
            m_linger = true;
        }

    }
    else if(strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        //处理Content-Length头部字段
        text+=15;
        text+= strspn(text," \t");
        m_content_length=atol(text);

    }
    else if(strncasecmp( text, "Host:", 5 ) == 0 )
    {
        //处理Host头部字段
        text+=5;
        text +=strspn(text," \t");
        m_host =text;
    }
    else
    {
      //  printf( "oop! unknow header %s\n", text );

    }
    return NO_REQUEST;

}
//没有真正解析HTTP请求的消息体，只是判断它是否被完整读入了
HTTP_CODE http_conn::parse_content(char*text)
{
    if(m_read_idx>=( m_content_length + m_checked_index ))
    {
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;



} 
//解析一行, 判断依据\r\n
LINE_STATUS http_conn::parse_line()
{
    char temp;
   // printf("当前读取位置%d\n",m_checked_index);
    //printf("最终读取位置%d\n",m_read_idx);
    for(;m_checked_index<m_read_idx;++m_checked_index)
    {
        temp =m_read_buf[m_checked_index];
        if(temp=='\r')
        {
            //printf("检测到\\r\n");
            if((m_checked_index+1)==m_read_idx)
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_index+1]=='\n')
            {
                m_read_buf[m_checked_index++]='\0';
                m_read_buf[m_checked_index++]='\0';
               // printf("检测到\\r\\n\n");
                return LINE_OK;
            }
            return LINE_BAD;



        }else if(temp=='\n')
        {
            if((m_checked_index>1)&&(m_read_buf[m_checked_index-1]=='\r'))
            {
                m_read_buf[m_checked_index-1]='\0';
                m_read_buf[m_checked_index++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
       

    }
    return LINE_OPEN;



}  
//当得到一个完整正确的HTTP请求时，我们就分析目标文件的属性
//如果目标文件存在，对所有用户可读，且不是目录,则使用mmap将其
//映射到内存地址m_file_address处，并告诉调用者获取文件成功
HTTP_CODE http_conn::do_request()
{
    // "/home/zhangyan/webserver/resources"
    strcpy(m_real_file,doc_root);
    int len= strlen(doc_root);
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if(stat(m_real_file,&m_file_stat)<0)
    {
        return NO_RESOURCE;
    }
    //判断访问权限
    if(!(m_file_stat.st_mode&S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    //以只读方式打开文件
    int fd =open(m_real_file,O_RDONLY);
    //创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close(fd);
    return FILE_REQUEST;
}
//由线程池的工作线程调用，这是处理HTTP请求的入口函数
void http_conn:: process()
{
    
    //解析http请求
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return ;
    }
    printf("parse request,create response\n");

    //生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);

}

//解除内存映射关系
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }

}

//写 HTTP响应

bool http_conn::write()
{
    int temp = 0;
    bytes_have_send = 0;            //已经发送的字节
    bytes_to_send = m_write_idx;    //将要发送的字节(m_write_idx)写缓冲区待发送的 字节数
    if (bytes_to_send==0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }
    while(1)
    {
        //分散写，每一次write都会有系统调用的开销，但是使用writev是不涉及多次的系统调用的
        //发送顺序是从第一个内存块开始，线性发送
        temp=writev(m_sockfd,m_iv,m_iv_count);
        if(temp<=-1)
        {
                                    //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。
                                    //服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性
            if(errno==EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        //如果头已经发送完毕,每一次调用write_v都会尝试将m_iv的所有内存块写入sockfd，即writev是无状态的
        if(bytes_have_send>=m_iv[0].iov_len)
        {
            m_iv[0].iov_len=0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send-m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else{
            m_iv[0].iov_base=m_write_buf + bytes_have_send;
            m_iv[0].iov_len=m_iv[0].iov_len-temp;
        }
        if(bytes_to_send <=0)
        {
            //发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger)
            {
                init();
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }
            else{
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return false;

            }
        }
        
    }

}


//往写缓冲区写入待发送的数据
bool http_conn::add_response(const char* format,...)
{
    if(m_write_idx>= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len =vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len>= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    //所有的内存块的总大小在add_response时叠加
    m_write_idx+=len;
    va_end(arg_list);
    return true;

}

bool http_conn::add_status_line(int status,const char* title)
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len)
{
    if(add_content_length(content_len)&&add_content_type()&&add_linger()&&add_blank_line())
    {
        return true;
    }
    else{
        return false;
    }
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n",content_len);
}

bool http_conn:: add_content_type()
{
    return add_response("Content-Type: %s\r\n", "text/html");
}
bool http_conn:: add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}
bool http_conn:: add_blank_line()
{
     return add_response( "%s", "\r\n" );
}
bool http_conn::add_content(const char* content)
{
    return add_response("%s",content);

}



//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
            {
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
            add_status_line(200,ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base=m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base= m_file_address;
            m_iv[1].iov_len =m_file_stat.st_size;
            m_iv_count =2;
            bytes_to_send = m_write_idx+m_file_stat.st_size;
            return true;
        default:
            return false;



    }
    m_iv[0].iov_base=m_write_buf;
    m_iv[0].iov_len=m_write_idx;
    m_iv_count=1;
    return true;
    
}