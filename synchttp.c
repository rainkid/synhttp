/*
HTTP Simple Sync Service - synchttp v1.0
Author: Rainkid, E-mail: rainkid@163.com
This is free software, and you are welcome to modify and redistribute it under the New BSD License
*/

#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>

#include <err.h>
#include <event.h>
#include <evhttp.h>

#include <tcbdb.h>

#include <curl/curl.h>
#include <curl/easy.h>

#define SYNHTTP_DEFAULT_MAXQUEUE 1000000
#define VERSION 1.0

/* 全局设置 */
TCBDB *synchttp_db_tcbdb; /* 数据表 */
int synchttp_settings_syncinterval; /* 同步更新内容到磁盘的间隔时间 */
char *synchttp_settings_pidfile; /* PID文件 */
char *synchttp_settings_auth; /* 验证密码 */

typedef struct SYNCHTTP_QUEUE{
	 char *method;
   	 char *queue_name;
   	 char *queue_url;
}SYNCHTTP_QUEUE;
/*消息队列*/
struct QUEUE_ITEM{
	SYNCHTTP_QUEUE * q;
    TAILQ_ENTRY(QUEUE_ITEM) entries;
};
/*消息队列头*/
TAILQ_HEAD(,QUEUE_ITEM) queue_head;

/* 创建多层目录的函数 */
void create_multilayer_dir( char *muldir )
{
    int    i,len;
    char    str[512];

    strncpy( str, muldir, 512 );
    len=strlen(str);
    for( i=0; i<len; i++ )
    {
        if( str[i]=='/' )
        {
            str[i] = '\0';
            //判断此目录是否存在,不存在则创建
            if( access(str, F_OK)!=0 )
            {
                mkdir( str, 0777 );
            }
            str[i]='/';
        }
    }
    if( len>0 && access(str, F_OK)!=0 )
    {
        mkdir( str, 0777 );
    }

    return;
}

int count_array(char *arr[], int size)
{
	int n_num = 0, n_size = 0;
	while(n_size != size)
	{
		n_size += sizeof(arr[n_num]);
		n_num++;
	}
	return n_num;
}

/*url解析函数*/
char *urldecode(char *input_str)
{
		int len = strlen(input_str);
		char *str = strdup(input_str);

        char *dest = str;
        char *data = str;

        int value;
        int c;

        while (len--) {
                if (*data == '+') {
                        *dest = ' ';
                }
                else if (*data == '%' && len >= 2 && isxdigit((int) *(data + 1))
  && isxdigit((int) *(data + 2)))
                {

                        c = ((unsigned char *)(data+1))[0];
                        if (isupper(c))
                                c = tolower(c);
                        value = (c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10) * 16;
                        c = ((unsigned char *)(data+1))[1];
                        if (isupper(c))
                                c = tolower(c);
                                value += c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;

                        *dest = (char)value ;
                        data += 2;
                        len -= 2;
                } else {
                        *dest = *data;
                }
                data++;
                dest++;
        }
        *dest = '\0';
        return str;
}

/*消息清除*/
static int synchttp_remove(const char *synchttp_input_name){
	char queue_name[300] = {0x00};
	sprintf(queue_name, "%s", synchttp_input_name);
	tcbdbout2(synchttp_db_tcbdb, queue_name);
	return 0;
}


struct SynhttpResponseStruct {
  char *responsetext;
  size_t size;
};

static size_t synchttp_write_callback(void *ptr, size_t size, size_t nmemb, void *data){
	size_t realsize = size * nmemb;
	struct SynhttpResponseStruct *mem = (struct SynhttpResponseStruct *)data;

	mem->responsetext = realloc(mem->responsetext, mem->size + realsize + 1);
	if (mem->responsetext == NULL) {
		/* out of memory! */
		exit(EXIT_FAILURE);
	}

	memcpy(&(mem->responsetext[mem->size]), ptr, realsize);
	mem->size += realsize;
	mem->responsetext[mem->size] = 0;

	return realsize;
}

/*消息分发*/
static int synchttp_dispense(SYNCHTTP_QUEUE *queue){
	char queue_name[300] = {0x00};
	char queue_url[300] = {0x00};
	int queue_value = 0;
	char *queue_value_tmp;

	CURL *synchttp_curl_handle = NULL;
	CURLcode response;

	int retval = 1;

	struct SynhttpResponseStruct chunk;

	chunk.responsetext = malloc(1);  /* will be grown as needed by the realloc above */
	chunk.size = 0;

	/*根据key获取队列数据*/
	sprintf(queue_name, "%s", queue->queue_name);
	sprintf(queue_url, "%s?", queue->queue_url);

	queue_value_tmp = tcbdbget2(synchttp_db_tcbdb, queue_name);
	if(queue_value_tmp != NULL){
		/*curl 选项设置*/
		synchttp_curl_handle = curl_easy_init();
		if(synchttp_curl_handle != NULL) {

				if(strcmp(queue->method, "get") == 0){
					strcat(queue_url, queue_value_tmp);
					printf("get : %s\n", queue_url);
				}

		       curl_easy_setopt(synchttp_curl_handle, CURLOPT_URL, queue_url);
		       if(strcmp(queue->method, "post") == 0){
		    	     printf("post : %s\n", queue_url);
			         curl_easy_setopt(synchttp_curl_handle, CURLOPT_POST, 1);
					 curl_easy_setopt(synchttp_curl_handle, CURLOPT_POSTFIELDS, queue_value_tmp);
		       }

		       curl_easy_setopt(synchttp_curl_handle, CURLOPT_WRITEFUNCTION, synchttp_write_callback);
		       curl_easy_setopt(synchttp_curl_handle, CURLOPT_WRITEDATA, &chunk);
		       response = curl_easy_perform(synchttp_curl_handle);
		}
		if((response == CURLE_OK) && chunk.responsetext && (strcmp(chunk.responsetext, "SYNCHTTP_SYNC_SUCCESS") == 0) ){
			fprintf(stderr, "Response is success\n");
			retval = 0;
		}else{
			fprintf(stderr, "Response is fail\n");
			retval = 1;
		}

		fprintf(stderr, "responsetext is %s \n", chunk.responsetext);

		if(chunk.responsetext){
			free(chunk.responsetext);
		}
		curl_easy_cleanup(synchttp_curl_handle);
		printf("%s => %s\n",queue_name, queue_value_tmp);
	}else{
		return 1;
	}
	free(queue_value_tmp);
	return 0;
}

/*实时监听消息队列，并发送处理请求*/
static void synchttp_dispatch(){
	pthread_detach(pthread_self());
	struct QUEUE_ITEM *temp_item;
	while(1){
		/*从消息队列头部开始取数据*/
		temp_item = TAILQ_FIRST(&queue_head);

		struct SYNCHTTP_QUEUE *queue;

		while(temp_item != NULL){
			queue = tccalloc(1, sizeof(queue));
			queue = temp_item->q;
			if(synchttp_dispense(queue) == 0){
				/*分发成功后赶出消息队列*/
				TAILQ_REMOVE(&queue_head, temp_item, entries);
				synchttp_remove((char *)temp_item->q->queue_name);
			}else{
				/*未分发成功保留在消息队列中*/
			}
			temp_item = TAILQ_NEXT(temp_item, entries);
		}
	}
}

/* 子进程信号处理 */
static void kill_signal_worker(const int sig) {
    /* 同步内存数据到磁盘，并关闭数据库 */
    tcbdbsync(synchttp_db_tcbdb);
    tcbdbclose(synchttp_db_tcbdb);
    tcbdbdel(synchttp_db_tcbdb);

    /*curl全局清空*/
    curl_global_cleanup();

    exit(0);
}

/* 父进程信号处理 */
static void kill_signal_master(const int sig) {
    /* 删除PID文件 */
    remove(synchttp_settings_pidfile);

    /* 给进程组发送SIGTERM信号，结束子进程 */
    kill(0, SIGTERM);

    exit(0);
}

/* 定时同步线程，定时将内存中的内容写入磁盘 */
static void synchttp_worker(const int sig) {
	pthread_detach(pthread_self());

	while(1)
	{
		/* 间隔synchttp_settings_syncinterval秒同步一次数据到磁盘 */
		sleep(synchttp_settings_syncinterval);

		/* 同步内存数据到磁盘 */
		tcbdbsync(synchttp_db_tcbdb);
	}
}

/* 处理模块 */
void synchttp_handler(struct evhttp_request *req, void *arg)
{
        struct evbuffer *buf;
        buf = evbuffer_new();
        time_t now = time(NULL);

		/* 分析URL参数 */
		char *decode_uri = strdup((char*) evhttp_request_uri(req));
		struct evkeyvalq synchttp_http_query;
		evhttp_parse_query(decode_uri, &synchttp_http_query);
		free(decode_uri);

		/* 接收GET表单参数 */
		const char *synchttp_input_name= evhttp_find_header (&synchttp_http_query, "name"); /*名称*/
		const char *synchttp_input_charset = evhttp_find_header (&synchttp_http_query, "charset"); /* 编码方式 */
		const char *synchttp_input_url = evhttp_find_header (&synchttp_http_query, "url"); /* 数据 */
		const char *synchttp_input_data = evhttp_find_header (&synchttp_http_query, "data"); /* 数据 */
		const char *synchttp_input_auth = evhttp_find_header (&synchttp_http_query, "auth"); /* 验证密码 */

		/* 返回给用户的Header头信息 */
		if (synchttp_input_charset != NULL && strlen(synchttp_input_charset) <= 40) {
			char content_type[64] = {0x00};
			sprintf(content_type, "text/plain; charset=%s", synchttp_input_charset);
			evhttp_add_header(req->output_headers, "Content-Type", content_type);
		} else {
			evhttp_add_header(req->output_headers, "Content-Type", "text/plain");
		}
		evhttp_add_header(req->output_headers, "Connection", "keep-alive");
		evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");
		//evhttp_add_header(req->output_headers, "Connection", "close");

		/* 权限校验 */
		bool is_auth_pass = false; /* 是否验证通过 */
		if (synchttp_settings_auth != NULL) {
			/* 如果命令行启动参数设置了验证密码 */
			if (synchttp_input_auth != NULL && strcmp(synchttp_settings_auth, synchttp_input_auth) == 0) {
				is_auth_pass = true;
			} else {
				is_auth_pass = false;
			}
		} else {
			/* 如果命令行启动参数没有设置验证密码 */
			is_auth_pass = true;
		}
		if (is_auth_pass == false){
			/* 校验失败 */
			evbuffer_add_printf(buf, "%s", "SYNHTTP_AUTH_FAILED");
		}else{
			/*参数是否存在判断 */
			if (synchttp_input_name != NULL && strlen(synchttp_input_name) <= 256) {
					int buffer_data_len;
					char queue_name[300] = {0x00}; /* 队列名称的总长度，用户输入的队列长度少于256字节 */
					char quene_name_temp[300] = {0x00};
					char quene_url_temp[300] = {0x00};

					sprintf(queue_name, "%s:%ld", synchttp_input_name, now);

					sprintf(quene_name_temp, "%s:%ld", synchttp_input_name, now);
					sprintf(quene_url_temp, "%s", synchttp_input_url);

					/*索引消息处理*/
					struct SYNCHTTP_QUEUE * queue;
					queue = tccalloc(1, sizeof(queue));
					queue->queue_name = quene_name_temp;
					queue->queue_url = quene_url_temp;

					/*请求消息入库*/
					char *synchttp_input_postbuffer;
					char *buffer_data;

					if (synchttp_input_data != NULL){
						/*GET请求*/
						queue->method = "get";
						buffer_data_len = strlen(synchttp_input_data);
						buffer_data = (char *)tccalloc(1, buffer_data_len + 1);
						memcpy (buffer_data, synchttp_input_data, buffer_data_len);
					}else{
						/*POST请求*/
						queue->method = "post";
						buffer_data_len = EVBUFFER_LENGTH(req->input_buffer);
						buffer_data = (char *)tccalloc(1, buffer_data_len + 1);
						memcpy (buffer_data, EVBUFFER_DATA(req->input_buffer), buffer_data_len);
					}
					synchttp_input_postbuffer = urldecode(buffer_data);

					struct QUEUE_ITEM *item;
					item= tccalloc(1, sizeof(item));
					item->q = queue;

					TAILQ_INSERT_TAIL(&queue_head, item, entries);

					tcbdbput2(synchttp_db_tcbdb, queue_name, synchttp_input_postbuffer);
					evbuffer_add_printf(buf, "%s", "SYNHTTP_SET_OK");
					free(synchttp_input_postbuffer);
					free(buffer_data);
			} else {
				/* 参数错误 */
				evbuffer_add_printf(buf, "%s", "SYNHTTP_ERROR");
			}
		}
		/* 输出内容给客户端 */
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		/* 内存释放 */
		evhttp_clear_headers(&synchttp_http_query);
		evbuffer_free(buf);
}


static void show_help(void)
{
	char *b="--------------------------------------------------------------------------------------------------\n"
		  "HTTP Simple Sync Service - synchttp v 1.0 (April 14, 2011)\n\n"
		  "Author: rainkid, zhuli ,  E-mail: rainkid@163.com\n"
		  "This is free software, and you are welcome to modify and redistribute it under the New BSD License\n"
		  "\n"
		   "-l <ip_addr>  interface to listen on, default is 0.0.0.0\n"
		   "-w <ip_addr>  interface to dispense out, default is 0.0.0.0\n"
		   "-p <num>      TCP port number to listen on (default: 2688)\n"
		   "-x <path>     database directory (example: /opt/synchttp/data)\n"
		   "-t <second>   keep-alive timeout for an http request (default: 60)\n"
		   "-s <second>   the interval to sync updated contents to the disk (default: 5)\n"
		   "-c <num>      the maximum number of non-leaf nodes to be cached (default: 1024)\n"
		   "-m <size>     database memory cache size in MB (default: 100)\n"
		   "-i <file>     save PID in <file> (default: /tmp/synchttp.pid)\n"
		   "-a <auth>     the auth password to access synchttp (example: mypasswd)\n"
		   "-d            run as a daemon\n"
		   "-h            print this help and exit\n\n"
		   "Use command \"killall synchttp\", \"pkill synchttp\" and \"kill `cat /tmp/synchttp.pid`\" to stop synchttp.\n"
		   "Please note that don't use the command \"pkill -9 synchttp\" and \"kill -9 PID of synchttp\"!\n"
		   "\n"
		   "Please visit \"https://github.com/rainkid/synchttp/\" for more help information.\n\n"
		   "--------------------------------------------------------------------------------------------------\n"
		   "\n";
	fprintf(stderr, b, strlen(b));
}

int main(int argc, char *argv[], char *envp[])
{
	int c;
	char *synchttp_settings_listen = "0.0.0.0";
	char *synchttp_sync_listen[] = {"0.0.0.0"};
	int synchttp_settings_port = 2688;
	char *synchttp_settings_datapath = NULL;
	bool synchttp_settings_daemon = false;
	int synchttp_settings_timeout = 60; /* 单位：秒 */
	synchttp_settings_syncinterval = 5; /* 单位：秒 */
	int synchttp_settings_cachenonleaf = 1024; /* 缓存非叶子节点数。单位：条 */
	int synchttp_settings_cacheleaf = 2048; /* 缓存叶子节点数。叶子节点缓存数为非叶子节点数的两倍。单位：条 */
	int synchttp_settings_mappedmemory = 104857600; /* 单位：字节 */
	synchttp_settings_pidfile = "/tmp/synchttp.pid";
	synchttp_settings_auth = NULL; /* 验证密码 */

  /* 启动选项 */
	while ((c = getopt(argc, argv, "l:p:x:t:s:c:m:i:a:w:dh")) != -1) {
		switch (c) {
		case 'l':
			synchttp_settings_listen = strdup(optarg);
			break;
		case 'p':
			synchttp_settings_port = atoi(optarg);
			break;
		case 'x':
			synchttp_settings_datapath = strdup(optarg); /* synchttp数据库文件存放路径 */
			if (access(synchttp_settings_datapath, W_OK) != 0) { /* 如果目录不可写 */
				if (access(synchttp_settings_datapath, R_OK) == 0) { /* 如果目录可读 */
					chmod(synchttp_settings_datapath, S_IWOTH); /* 设置其他用户具可写入权限 */
				} else { /* 如果不存在该目录，则创建 */
					create_multilayer_dir(synchttp_settings_datapath);
				}

				if (access(synchttp_settings_datapath, W_OK) != 0) { /* 如果目录不可写 */
					fprintf(stderr, "synchttp database directory not writable\n");
				}
			}
			break;
		case 't':
			synchttp_settings_timeout = atoi(optarg);
			break;
		case 's':
			synchttp_settings_syncinterval = atoi(optarg);
			break;
		case 'c':
			synchttp_settings_cachenonleaf = atoi(optarg);
			synchttp_settings_cacheleaf = synchttp_settings_cachenonleaf * 2;
			break;
		case 'm':
			synchttp_settings_mappedmemory = atoi(optarg) * 1024 * 1024; /* 单位：M */
			break;
		case 'i':
			synchttp_settings_pidfile = strdup(optarg);
			break;
		case 'a':
			synchttp_settings_auth = strdup(optarg);
			break;
		case 'w':
			synchttp_sync_listen[0] = strdup(optarg);
		case 'd':
			synchttp_settings_daemon = true;
			break;
		case 'h':
		default:
			show_help();
			return 1;
		}
	}

	/* 判断是否加了必填参数 -x */
	if (synchttp_settings_datapath == NULL) {
		show_help();
		fprintf(stderr, "Attention: Please use the indispensable argument: -x <path>\n\n");
		exit(1);
	}
 
  	/*curl初始化全局信息*/
  	if ((curl_global_init(CURL_GLOBAL_ALL)) != CURLE_OK) {
		fprintf(stderr, "Curl global init fail.\n");
		exit(1);
	}

	/* 数据表路径 */
	int synchttp_settings_dataname_len = 1024;
	char *synchttp_settings_dataname = (char *)tccalloc(1, synchttp_settings_dataname_len);
	sprintf(synchttp_settings_dataname, "%s/synchttp.db", synchttp_settings_datapath);

	/* 打开数据表 */
	synchttp_db_tcbdb = tcbdbnew();
	tcbdbsetmutex(synchttp_db_tcbdb); /* 开启线程互斥锁 */
	tcbdbtune(synchttp_db_tcbdb, 1024, 2048, 50000000, 8, 10, BDBTLARGE);
	tcbdbsetcache(synchttp_db_tcbdb, synchttp_settings_cacheleaf, synchttp_settings_cachenonleaf);
	tcbdbsetxmsiz(synchttp_db_tcbdb, synchttp_settings_mappedmemory); /* 内存缓存大小 */

	/* 判断表是否能打开 */
	if(!tcbdbopen(synchttp_db_tcbdb, synchttp_settings_dataname, BDBOWRITER|BDBOCREAT)){
		fprintf(stderr, "Attention: Unable to open the database.\n\n");
		exit(1);
	}
	/* 释放变量所占内存 */
	free(synchttp_settings_dataname);

	/* 如果加了-d参数，以守护进程运行 */
	if (synchttp_settings_daemon == true) {
		pid_t pid;

		/* Fork off the parent process */
		pid = fork();
		if (pid < 0) {
				exit(EXIT_FAILURE);
		}
		/* If we got a good PID, then
		   we can exit the parent process. */
		if (pid > 0) {
				exit(EXIT_SUCCESS);
		}
	}

	/* 将进程号写入PID文件 */
	FILE *fp_pidfile;
	fp_pidfile = fopen(synchttp_settings_pidfile, "w");
	fprintf(fp_pidfile, "%d\n", getpid());
	fclose(fp_pidfile);

	 /* 派生synchttp子进程（工作进程） */
	pid_t synchttp_worker_pid_wait;
	pid_t synchttp_worker_pid = fork();
	/* 如果派生进程失败，则退出程序 */
	if (synchttp_worker_pid < 0)
	{
		fprintf(stderr, "Error: %s:%d\n", __FILE__, __LINE__);
		exit(EXIT_FAILURE);
	}

	/* synchttp父进程内容 */
	if (synchttp_worker_pid > 0)
	{
		/* 处理父进程接收到的kill信号 */

		/* 忽略Broken Pipe信号 */
		signal(SIGPIPE, SIG_IGN);

		/* 处理kill信号 */
		signal (SIGINT, kill_signal_master);
		signal (SIGKILL, kill_signal_master);
		signal (SIGQUIT, kill_signal_master);
		signal (SIGTERM, kill_signal_master);
		signal (SIGHUP, kill_signal_master);

		/* 处理段错误信号 */
		signal(SIGSEGV, kill_signal_master);

		/* 如果子进程终止，则重新派生新的子进程 */
		while (1)
		{
			synchttp_worker_pid_wait = wait(NULL);
			if (synchttp_worker_pid_wait < 0)
			{
				continue;
			}
			usleep(100000);
			synchttp_worker_pid = fork();
			if (synchttp_worker_pid == 0)
			{
				break;
			}
		}
	}

	/*****************************************子进程处理************************************/
	/* 忽略Broken Pipe信号 */
	signal(SIGPIPE, SIG_IGN);

	/* 处理kill信号 */
	signal (SIGINT, kill_signal_worker);
	signal (SIGKILL, kill_signal_worker);
	signal (SIGQUIT, kill_signal_worker);
	signal (SIGTERM, kill_signal_worker);
	signal (SIGHUP, kill_signal_worker);

	/* 处理段错误信号 */
	signal(SIGSEGV, kill_signal_worker);


	/*创建消息队列监听进程*/
	TAILQ_INIT(&queue_head);
	pthread_t synchttp_dispatch_id;
	pthread_create(&synchttp_dispatch_id, NULL, (void *) synchttp_dispatch, NULL);

	/* 创建定时同步线程，定时将内存中的内容写入磁盘 */
	pthread_t synchttp_worker_tid;
	pthread_create(&synchttp_worker_tid, NULL, (void *) synchttp_worker, NULL);

	/* 外部请求处理部分 */
	struct evhttp *synchttpd;
	event_init();
	synchttpd = evhttp_start(synchttp_settings_listen, synchttp_settings_port);
	if (synchttpd == NULL) {
		fprintf(stderr, "Error: Unable to listen on %s:%d\n\n", synchttp_settings_listen, synchttp_settings_port);
		kill(0, SIGTERM);
		exit(1);
	}
	evhttp_set_timeout(synchttpd, synchttp_settings_timeout);
	evhttp_set_gencb(synchttpd, synchttp_handler, NULL);
	event_dispatch();
	evhttp_free(synchttpd);
}
