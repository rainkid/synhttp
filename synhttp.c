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

/* 全局设置 */
TCBDB *synhttp_db_tcbdb; /* 数据表 */
int synhttp_settings_syncinterval; /* 同步更新内容到磁盘的间隔时间 */
char *synhttp_settings_pidfile; /* PID文件 */
char *synhttp_settings_auth; /* 验证密码 */

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

/*实时监听消息队列，并发送处理请求*/
static void syn_dispatch(){
	pthread_detach(pthread_self());

	while(1){
		struct QUEUE_ITEM *temp_item;
		temp_item=TAILQ_FIRST(&queue_head);
		while(temp_item != NULL){
			printf("%s\n",temp_item->name);
			TAILQ_REMOVE(&queue_head, temp_item, entries);
			temp_item = TAILQ_NEXT(temp_item, entries);
		}
	}
}

/* 定时同步线程，定时将内存中的内容写入磁盘 */
static void sync_worker(const int sig) {
	pthread_detach(pthread_self());

	while(1)
	{
		/* 间隔synhttp_settings_syncinterval秒同步一次数据到磁盘 */
		sleep(synhttp_settings_syncinterval);

		/* 同步内存数据到磁盘 */
		tcbdbsync(synhttp_db_tcbdb);
	}
}

/* 获取本次“入队列”操作的队列写入点 */
static int synhttp_now_setpos(const char* httpsqs_input_name)
{
	return 1;
}

char * synhttp_get_inputname(char * synhttp_input_name){
	return synhttp_input_name;
}

/* 处理模块 */
void synhttp_handler(struct evhttp_request *req, void *arg)
{
        struct evbuffer *buf;
        buf = evbuffer_new();

		/* 分析URL参数 */
		char *decode_uri = strdup((char*) evhttp_request_uri(req));
		struct evkeyvalq synhttp_http_query;
		evhttp_parse_query(decode_uri, &synhttp_http_query);
		free(decode_uri);

		/* 接收GET表单参数 */
		const char *synhttp_input_name="synhttpqueue";
		const char *synhttp_input_charset = evhttp_find_header (&synhttp_http_query, "charset"); /* 操作类别 */
		const char *synhttp_input_opt = evhttp_find_header (&synhttp_http_query, "opt"); /* 操作类别 */
		const char *synhttp_input_data = evhttp_find_header (&synhttp_http_query, "data"); /* 消息数据 */
		const char *synhttp_input_auth = evhttp_find_header (&synhttp_http_query, "auth"); /* 验证密码 */

		/* 返回给用户的Header头信息 */
		if (synhttp_input_charset != NULL && strlen(synhttp_input_charset) <= 40) {
			char content_type[64] = {0};
			sprintf(content_type, "text/plain; charset=%s", synhttp_input_charset);
			evhttp_add_header(req->output_headers, "Content-Type", content_type);
		} else {
			evhttp_add_header(req->output_headers, "Content-Type", "text/plain");
		}
		evhttp_add_header(req->output_headers, "Connection", "keep-alive");
		evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");
		//evhttp_add_header(req->output_headers, "Connection", "close");

		/* 权限校验 */
		bool is_auth_pass = false; /* 是否验证通过 */
		if (synhttp_settings_auth != NULL) {
			/* 如果命令行启动参数设置了验证密码 */
			if (synhttp_input_auth != NULL && strcmp(synhttp_settings_auth, synhttp_input_auth) == 0) {
				is_auth_pass = true;
			} else {
				is_auth_pass = false;
			}
		} else {
			/* 如果命令行启动参数没有设置验证密码 */
			is_auth_pass = true;
		}

		if (is_auth_pass == false)
		{
			/* 校验失败 */
			evbuffer_add_printf(buf, "%s", "HTTPSQS_AUTH_FAILED");
			goto done;
		}

		/*参数是否存在判断 */
		if (synhttp_input_name != NULL && synhttp_input_opt != NULL && strlen(synhttp_input_name) <= 256) {
			/* 入队列 */
			if (strcmp(synhttp_input_opt, "set") != 0)
			{
				evbuffer_add_printf(buf, "%s", "HTTPSQS_PUT_ERROR");
				goto done;
			}

			/* 优先接收POST正文信息 */
			int buffer_data_len;
			if (synhttp_input_data != NULL){
				buffer_data_len = strlen(synhttp_input_data);
			}else{
				buffer_data_len = EVBUFFER_LENGTH(req->input_buffer);
			}
			int queue_set_value = synhttp_now_setpos((char *)synhttp_input_name);

			if (queue_set_value > 0)
			{
				evbuffer_add_printf(buf, "%s", "HTTPSQS_PUT_END");
				goto done;
			}
			char queue_name[300] = {0}; /* 队列名称的总长度，用户输入的队列长度少于256字节 */
			sprintf(queue_name, "%s:%d", synhttp_input_name, queue_set_value);

			/*索引消息处理*/
			struct QUEUE_ITEM *item;
			item=malloc(sizeof(item));
			item->name=queue_name;
			TAILQ_INSERT_TAIL(&queue_head, item, entries);

			/**/
			char *synhttp_input_postbuffer;
			char *buffer_data = (char *)tccalloc(1, buffer_data_len + 1);

			memcpy (buffer_data, EVBUFFER_DATA(req->input_buffer), buffer_data_len);
			synhttp_input_postbuffer = urldecode(buffer_data);
			tcbdbput2(synhttp_db_tcbdb, queue_name, synhttp_input_postbuffer);
			evbuffer_add_printf(buf, "%s", "HTTPSQS_PUT_OK");
			free(synhttp_input_postbuffer);
			free(buffer_data);

		} else {
			/* 命令错误 */
			evbuffer_add_printf(buf, "%s", "HTTPSQS_ERROR");
		}
done:
		/* 输出内容给客户端 */
        evhttp_send_reply(req, HTTP_OK, "OK", buf);
		/* 内存释放 */
		evhttp_clear_headers(&synhttp_http_query);
		evbuffer_free(buf);
}


int main(int argc, char *argv[], char *envp[])
{
	char *synhttp_settings_listen = "0.0.0.0";
	int synhttp_settings_port = 2688;
	char *synhttp_settings_datapath = "tmp";
	bool synhttp_settings_daemon = false;
	int synhttp_settings_timeout = 60; /* 单位：秒 */
	synhttp_settings_syncinterval = 5; /* 单位：秒 */
	int synhttp_settings_cachenonleaf = 1024; /* 缓存非叶子节点数。单位：条 */
	int synhttp_settings_cacheleaf = 2048; /* 缓存叶子节点数。叶子节点缓存数为非叶子节点数的两倍。单位：条 */
	int synhttp_settings_mappedmemory = 104857600; /* 单位：字节 */

	/* 数据表路径 */
	int synhttp_settings_dataname_len = 1024;
	char *synhttp_settings_dataname = (char *)tccalloc(1, synhttp_settings_dataname_len);
	sprintf(synhttp_settings_dataname, "%s/synhttp.db", synhttp_settings_datapath);

	/* 打开数据表 */
	synhttp_db_tcbdb = tcbdbnew();
	tcbdbsetmutex(synhttp_db_tcbdb); /* 开启线程互斥锁 */
	tcbdbtune(synhttp_db_tcbdb, 1024, 2048, 50000000, 8, 10, BDBTLARGE);
	tcbdbsetcache(synhttp_db_tcbdb, synhttp_settings_cacheleaf, synhttp_settings_cachenonleaf);
	tcbdbsetxmsiz(synhttp_db_tcbdb, synhttp_settings_mappedmemory); /* 内存缓存大小 */

	/* 判断表是否能打开 */
	if(!tcbdbopen(synhttp_db_tcbdb, synhttp_settings_dataname, BDBOWRITER|BDBOCREAT)){
		fprintf(stderr, "Attention: Unable to open the database.\n\n");
		exit(1);
	}
	/* 释放变量所占内存 */
	free(synhttp_settings_dataname);

	/*创建消息队列监听进程*/
	TAILQ_INIT(&queue_head);
	pthread_t syn_dispatch_id;
	pthread_create(&syn_dispatch_id, NULL, (void *) syn_dispatch, NULL);

	/* 创建定时同步线程，定时将内存中的内容写入磁盘 */
	pthread_t sync_worker_tid;
	pthread_create(&sync_worker_tid, NULL, (void *) sync_worker, NULL);

	/* 外部请求处理部分 */
	struct evhttp *httpd;
	event_init();
	httpd = evhttp_start(synhttp_settings_listen, synhttp_settings_port);
	if (httpd == NULL) {
		fprintf(stderr, "Error: Unable to listen on %s:%d\n\n", synhttp_settings_listen, synhttp_settings_port);
		kill(0, SIGTERM);
		exit(1);
	}
	evhttp_set_timeout(httpd, synhttp_settings_timeout);
	evhttp_set_gencb(httpd, synhttp_handler, NULL);
	event_dispatch();
	evhttp_free(httpd);

}
