/*
HTTP Simple Sync Service - synhttp v1.0
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

/* 全局设置 */
TCBDB *synchttp_db_tcbdb; /* 数据表 */
int synchttp_settings_syncinterval; /* 同步更新内容到磁盘的间隔时间 */
char *synchttp_settings_pidfile; /* PID文件 */
char *synchttp_settings_auth; /* 验证密码 */

/*消息队列*/
struct QUEUE_ITEM{
     char *name;
     TAILQ_ENTRY(QUEUE_ITEM) entries;
};
/*消息队列头*/
TAILQ_HEAD(,QUEUE_ITEM) queue_head;

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

static int synchttp_remove(const char *synchttp_input_name){
	char queue_name[300] = {0};
	sprintf(queue_name, "%s", synchttp_input_name);
	tcbdbout2(synchttp_db_tcbdb, queue_name);
	tcbdbsync(synchttp_db_tcbdb); /* 实时刷新到磁盘 */
	return 0;
}

static int synchttp_dispense(const char * synhttp_input_name){
	char queue_name[300] = {0};

	int queue_value = 0;
	char *queue_value_tmp;

	sprintf(queue_name, "%s", synhttp_input_name);

	queue_value_tmp = tcbdbget2(synchttp_db_tcbdb, queue_name);
	if(queue_value_tmp != NULL){
		printf("%s => %s\n",queue_name, queue_value_tmp);
	}else{
		return 1;
	}

	free(queue_value_tmp);
	return 0;
}

/*实时监听消息队列，并发送处理请求*/
static void synchttp_dispatch(){
	char queue_name[300] = {0};
	pthread_detach(pthread_self());
	while(1){
		/*从消息队列头部开始取数据*/
		struct QUEUE_ITEM *temp_item;
		temp_item=TAILQ_FIRST(&queue_head);

		while(temp_item != NULL){
			memset(queue_name, '\0', 300);
			sprintf(queue_name, "%s", temp_item->name);

			if(synchttp_dispense(queue_name) == 0){
				/*分发成功后赶出消息队列*/
				TAILQ_REMOVE(&queue_head, temp_item, entries);
				synchttp_remove((char *)queue_name);
			}else{
				/*未分发成功保留在消息队列中*/

			}
			temp_item = TAILQ_NEXT(temp_item, entries);
		}
	}
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
		const char *synchttp_input_charset = evhttp_find_header (&synchttp_http_query, "charset"); /* 操作类别 */
		const char *synchttp_input_opt = evhttp_find_header (&synchttp_http_query, "opt"); /* 操作类别 */
		const char *synchttp_input_data = evhttp_find_header (&synchttp_http_query, "data"); /* 数据 */
		const char *synchttp_input_auth = evhttp_find_header (&synchttp_http_query, "auth"); /* 验证密码 */

		/* 返回给用户的Header头信息 */
		if (synchttp_input_charset != NULL && strlen(synchttp_input_charset) <= 40) {
			char content_type[64] = {0};
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
			if (synchttp_input_name != NULL && synchttp_input_opt != NULL && strlen(synchttp_input_name) <= 256) {
				if (strcmp(synchttp_input_opt, "set") == 0){
					int buffer_data_len;
					char queue_name[300] = {0}; /* 队列名称的总长度，用户输入的队列长度少于256字节 */
					char quene_name_temp[300] = {0};
					sprintf(queue_name, "%s", "testqueue");
					sprintf(quene_name_temp, "%s", "testqueue");

					/*索引消息处理*/
					struct QUEUE_ITEM *item;
					item=malloc(sizeof(item));
					item->name = quene_name_temp;
					TAILQ_INSERT_TAIL(&queue_head, item, entries);

					/*请求消息入库*/
					char *synchttp_input_postbuffer;
					char *buffer_data = (char *)tccalloc(1, buffer_data_len + 1);

					if (synchttp_input_data != NULL){
						/*GET请求*/
						buffer_data_len = strlen(synchttp_input_data);
						memcpy (buffer_data, synchttp_input_data, buffer_data_len);
					}else{
						/*POST请求*/
						buffer_data_len = EVBUFFER_LENGTH(req->input_buffer);
						memcpy (buffer_data, EVBUFFER_DATA(req->input_buffer), buffer_data_len);
					}
					synchttp_input_postbuffer = urldecode(buffer_data);

					tcbdbput2(synchttp_db_tcbdb, queue_name, synchttp_input_postbuffer);
					evbuffer_add_printf(buf, "%s", "SYNHTTP_SET_OK");
					free(synchttp_input_postbuffer);
					free(buffer_data);
				}else{
					evbuffer_add_printf(buf, "%s", "SYNHTTP_SET_ERROR");
				}
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


int main(int argc, char *argv[], char *envp[])
{
	char *synchttp_settings_listen = "0.0.0.0";
	int synchttp_settings_port = 2688;
	char *synchttp_settings_datapath = "tmp";
	bool synchttp_settings_daemon = false;
	int synchttp_settings_timeout = 60; /* 单位：秒 */
	synchttp_settings_syncinterval = 5; /* 单位：秒 */
	int synchttp_settings_cachenonleaf = 1024; /* 缓存非叶子节点数。单位：条 */
	int synchttp_settings_cacheleaf = 2048; /* 缓存叶子节点数。叶子节点缓存数为非叶子节点数的两倍。单位：条 */
	int synchttp_settings_mappedmemory = 104857600; /* 单位：字节 */

	/* 数据表路径 */
	int synchttp_settings_dataname_len = 1024;
	char *synchttp_settings_dataname = (char *)tccalloc(1, synchttp_settings_dataname_len);
	sprintf(synchttp_settings_dataname, "%s/synhttp.db", synchttp_settings_datapath);

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
