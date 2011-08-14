/*
 HTTP Simple Sync Service - synchttp v1.0
 Author: Rainkid, E-mail: rainkid@163.com
 This is free software, and you are welcome to modify and redistribute it under the New BSD License
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <getopt.h>
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

#include "lib/tool.h"

#define SYNCHTTP_DEFAULT_MAXQUEUE 1000000
#define VERSION 1.0

/* 全局设置 */
TCBDB *synchttp_db_tcbdb; /* 数据表 */
int synchttp_settings_syncinterval; /* 同步更新内容到磁盘的间隔时间 */
char *synchttp_settings_pidfile; /* PID文件 */
char *synchttp_settings_auth; /* 验证密码 */

int num = 0;

//获取队列写入点
static int synchttp_get_putpos(){
	int putpos_value = 0;
	char *putpos_value_tmp;
	char queue_name[32] = {0};

	sprintf(queue_name, "%s", "PP");
	putpos_value_tmp = tcbdbget2(synchttp_db_tcbdb, queue_name);
	if(putpos_value_tmp){
		putpos_value = atoi(putpos_value_tmp);
		free(putpos_value_tmp);
	}

	return putpos_value;
}

//获取队列读取点
static int synchttp_get_readpos()
{
	int readpos_value = 0;
	char *readpos_value_tmp;
	char queue_name[32] = {0};

	readpos_value_tmp = tcbdbget2(synchttp_db_tcbdb, "RP");
	if(readpos_value_tmp){
		readpos_value = atoi(readpos_value_tmp);
		free(readpos_value_tmp);
	}

	return readpos_value;
}

//设置队列写入点
static int synchttp_now_putpos()
{
	int readpos_value = 0;
	int putpos_value = 0;
	char queue_input[32] = {0};

	readpos_value = synchttp_get_readpos();
	putpos_value = synchttp_get_putpos();

	putpos_value = putpos_value + 1;
	if(putpos_value == readpos_value){
		//消息队列已满
		putpos_value = 0;
	}else if(readpos_value <=0 && putpos_value > SYNCHTTP_DEFAULT_MAXQUEUE){
		//如果队列没有出操作并且队列已满
		putpos_value = 0;
	}else if(putpos_value > SYNCHTTP_DEFAULT_MAXQUEUE){
		if(tcbdbput2(synchttp_db_tcbdb, "PP", "1")) {
			putpos_value = 1;
		}
	}else{
		sprintf(queue_input,"%d", putpos_value);
		tcbdbput2(synchttp_db_tcbdb, "PP", (char *)queue_input);
	}
	return putpos_value;
}


static int synchttp_now_readpos()
{
	int putpos_value = 0;
	int readpos_value = 0;
	char queue_name[300] = {0}; /* 队列名称的总长度，用户输入的队列长度少于256字节 */

	/* 读取当前队列写入位置点 */
	putpos_value = synchttp_get_putpos();

	/* 读取当前队列读取位置点 */
	readpos_value = synchttp_get_readpos();

	/* 如果readpos_value的值不存在，重置为1 */
	if (readpos_value == 0 && putpos_value > 0) {
		readpos_value = 1;
		tcbdbput2(synchttp_db_tcbdb, "RP", "1");
	/* 如果队列的读取值（出队列）小于队列的写入值（入队列） */
	} else if (readpos_value < putpos_value) {
		readpos_value = readpos_value + 1;
		char queue_input[32] = {0};
		sprintf(queue_input, "%d", readpos_value);
		tcbdbput2(synchttp_db_tcbdb, "RP", queue_input);
	/* 如果队列的读取值（出队列）大于队列的写入值（入队列），并且队列的读取值（出队列）小于最大队列数量 */
	} else if (readpos_value > putpos_value && readpos_value < SYNCHTTP_DEFAULT_MAXQUEUE) {
		readpos_value = readpos_value + 1;
		char queue_input[32] = {0};
		sprintf(queue_input, "%d", readpos_value);
		tcbdbput2(synchttp_db_tcbdb, "RP", queue_input);
	/* 如果队列的读取值（出队列）大于队列的写入值（入队列），并且队列的读取值（出队列）等于最大队列数量 */
	} else if (readpos_value > putpos_value && readpos_value == SYNCHTTP_DEFAULT_MAXQUEUE) {
		readpos_value = 1;
		tcbdbput2(synchttp_db_tcbdb, "RP", "1");
	/* 队列的读取值（出队列）等于队列的写入值（入队列），即队列中的数据已全部读出 */
	} else {
		readpos_value = 0;
	}
	return readpos_value;
}

struct SynhttpResponseStruct {
	char *responsetext;
	size_t size;
};

static size_t synchttp_write_callback(void *ptr, size_t size, size_t nmemb, void *data) {
	size_t realsize = size * nmemb;
	struct SynhttpResponseStruct *mem = (struct SynhttpResponseStruct *) data;

	mem->responsetext = realloc(mem->responsetext, mem->size + realsize + 1);
	if (mem->responsetext == NULL) {
		exit(EXIT_FAILURE);
	}

	memcpy(&(mem->responsetext[mem->size]), ptr, realsize);
	mem->size += realsize;
	mem->responsetext[mem->size] = 0;
	return realsize;
}

static int synchttp_request(char *synchttp_queue_data) {
	CURL *synchttp_curl_handle = NULL;
	CURLcode response;
	int retval = 1;
	char *queue_url,*queue_data, *queue_method;

	struct SynhttpResponseStruct chunk;
	chunk.responsetext = malloc(1);
	chunk.size = 0;

	int len = strlen(synchttp_queue_data);
	queue_method = strdup(substr(synchttp_queue_data, len-2,len));

	/*curl 选项设置*/
	synchttp_curl_handle = curl_easy_init();
	if (synchttp_curl_handle != NULL) {
		num++;
		/*get请求*/
		if (strcmp(queue_method, "#1") == 0) {
			queue_url = tccalloc(1, strlen(synchttp_queue_data) + 1);
			memcpy(queue_url, synchttp_queue_data, strlen(synchttp_queue_data)-2);
			fprintf(stderr, "[ %d ]get:%s        ",num, queue_url);
		}
		/*post请求*/
		if (strcmp(queue_method, "#2") == 0) {
			int query_url_len = strcspn(synchttp_queue_data,"?");
			queue_url = strdup(substr(synchttp_queue_data, 0, query_url_len));
			queue_data = strdup(substr(synchttp_queue_data, query_url_len+1, len-2));
			fprintf(stderr, "post:%s        ", queue_url);
			curl_easy_setopt(synchttp_curl_handle, CURLOPT_POST, 1);
			curl_easy_setopt(synchttp_curl_handle, CURLOPT_POSTFIELDS, queue_data);
		}
		curl_easy_setopt(synchttp_curl_handle, CURLOPT_URL, queue_url);
		/*回调设置*/
		curl_easy_setopt(synchttp_curl_handle, CURLOPT_WRITEFUNCTION, synchttp_write_callback);
		curl_easy_setopt(synchttp_curl_handle, CURLOPT_WRITEDATA, &chunk);
		response = curl_easy_perform(synchttp_curl_handle);
	}

	/*请求响应处理*/
	if ((response == CURLE_OK) && chunk.responsetext && (strcmp(chunk.responsetext, "SYNCHTTP_SYNC_SUCCESS") == 0)) {
		fprintf(stderr, "[success]\n");
		retval = 0;
	} else {
		fprintf(stderr, "[fail]\n");
		retval = 1;
	}
	if (chunk.responsetext) {
		free(chunk.responsetext);
	}
	curl_easy_cleanup(synchttp_curl_handle);
	free(queue_url);
	free(queue_data);
	free(queue_method);
}

/*消息分发*/
static bool synchttp_dispense(const char *synchttp_queue_name) {
	char *queue_data;
	char *token;
	char *sync_listen;
	char queue_name[32] = {0x00};
	sprintf(queue_name, "%s", synchttp_queue_name);
	queue_data = tcbdbget2(synchttp_db_tcbdb, queue_name);
	if (queue_data != NULL) {
		synchttp_request(queue_data);
	}
	free(token);
	free(sync_listen);
	free(queue_data);
	return false;
}

/*实时监听消息队列，并发送处理请求*/
static void synchttp_dispatch() {
	pthread_detach(pthread_self());

	char queue_name[300] = {0x00};
	int readpos_value = 0;

	while (1) {
		readpos_value = synchttp_now_readpos();
		if(readpos_value >0){
			sprintf(queue_name, "Q:%d", readpos_value);
			if(synchttp_dispense(queue_name) == true){

			}
			tcbdbout2(synchttp_db_tcbdb, queue_name);
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

	while (1) {
		/* 间隔synchttp_settings_syncinterval秒同步一次数据到磁盘 */
		sleep(synchttp_settings_syncinterval);

		/* 同步内存数据到磁盘 */
		tcbdbsync(synchttp_db_tcbdb);
	}
}

/* 处理模块 */
void synchttp_handler(struct evhttp_request *req, void *arg) {
	struct evbuffer *buf;
	buf = evbuffer_new();

	/* 分析URL参数 */
	char *decode_uri = strdup((char*) evhttp_request_uri(req));
	struct evkeyvalq synchttp_http_query;
	evhttp_parse_query(decode_uri, &synchttp_http_query);
	free(decode_uri);

	/* 接收GET表单参数 */
	const char *synchttp_input_charset = evhttp_find_header(&synchttp_http_query, "charset"); /* 编码方式 */
	const char *synchttp_input_data = evhttp_find_header(&synchttp_http_query, "data"); /* 数据 */
	const char *synchttp_input_auth = evhttp_find_header(&synchttp_http_query, "auth"); /* 验证密码 */

	/* 返回给用户的Header头信息 */
	if (synchttp_input_charset != NULL && strlen(synchttp_input_charset) <= 40) {
		char content_type[64] = { 0x00 };
		sprintf(content_type, "text/plain; charset=%s", synchttp_input_charset);
		evhttp_add_header(req->output_headers, "Content-Type", content_type);
	} else {
		evhttp_add_header(req->output_headers, "Content-Type", "text/plain");
	}
	evhttp_add_header(req->output_headers, "Connection", "keep-alive");
	evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");

	/* 权限校验 */
	bool is_auth_pass = false;
	/* 是否验证通过 */
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
	if (is_auth_pass == false) {
		/* 校验失败 */
		evbuffer_add_printf(buf, "%s", "SYNCHTTP_AUTH_FAILED");
		goto output;
	}

	int now_putpos = synchttp_now_putpos();
	if(now_putpos > SYNCHTTP_DEFAULT_MAXQUEUE){
		evbuffer_add_printf(buf, "%s", "SYNCHTTP_PUT_FULL");
		goto output;
	}

	int buffer_data_len;
	char queue_name[32] = { 0x00 };
	sprintf(queue_name,"Q:%d", now_putpos);

	/*请求消息入库*/
	char *synchttp_input_buffer;
	char *buffer_data;
	if (synchttp_input_data != NULL) {
		/*GET请求*/
		buffer_data_len = strlen(synchttp_input_data);
		buffer_data = (char *) tccalloc(1, buffer_data_len + 3);
		memcpy(buffer_data, synchttp_input_data, buffer_data_len);
		strcat(buffer_data, "#1");
	} else {
		/*POST请求*/
		buffer_data_len = EVBUFFER_LENGTH(req->input_buffer);
		buffer_data = (char *) tccalloc(1, buffer_data_len + 3);
		memcpy(buffer_data, EVBUFFER_DATA(req->input_buffer), buffer_data_len);
		strcat(buffer_data, "#2");
	}
	synchttp_input_buffer = urldecode(buffer_data);
	/*参数是否存在判断 */
	if (strlen(synchttp_input_buffer) < 3){
		/* 参数错误 */
		evbuffer_add_printf(buf, "%s", "SYNCHTTP_ERROR");
		goto output;
	}
	if(tcbdbput2(synchttp_db_tcbdb, queue_name, synchttp_input_buffer) == true){
		fprintf(stderr, "in:%s--->%s\n", queue_name, synchttp_input_buffer);
	}else{
		evbuffer_add_printf(buf, "%s", "SYNCHTTP_ERROR");
		goto output;
	}
	evbuffer_add_printf(buf, "%s", "SYNCHTTP_SET_OK");
	free(synchttp_input_buffer);
	free(buffer_data);

output:
	/* 输出内容给客户端 */
	evhttp_send_reply(req, HTTP_OK, "OK", buf);
	/* 内存释放 */
	evhttp_clear_headers(&synchttp_http_query);
	evbuffer_free(buf);
}

static void show_help(void) {
	char    *b ="--------------------------------------------------------------------------------------------------\n"
				"HTTP Simple Sync Service - synchttp v 1.0 (April 14, 2011)\n\n"
				"Author: rainkid, zhuli ,  E-mail: rainkid@163.com\n"
				"This is free software, and you are welcome to modify and redistribute it under the New BSD License\n"
				"\n"
				"-l <ip_addr>  interface to listen on, default is 0.0.0.0\n"
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

int main(int argc, char *argv[], char *envp[]) {

	int c;
	char *synchttp_settings_listen = "0.0.0.0";
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
	char *synchttp_settings_dataname = (char *) tccalloc(1, synchttp_settings_dataname_len);
	sprintf(synchttp_settings_dataname, "%s/synchttp.db", synchttp_settings_datapath);

	/* 打开数据表 */
	synchttp_db_tcbdb = tcbdbnew();
	tcbdbsetmutex(synchttp_db_tcbdb); /* 开启线程互斥锁 */
	tcbdbtune(synchttp_db_tcbdb, 1024, 2048, 50000000, 8, 10, BDBTLARGE);
	tcbdbsetcache(synchttp_db_tcbdb, synchttp_settings_cacheleaf,
			synchttp_settings_cachenonleaf);
	tcbdbsetxmsiz(synchttp_db_tcbdb, synchttp_settings_mappedmemory); /* 内存缓存大小 */

	/* 判断表是否能打开 */
	if (!tcbdbopen(synchttp_db_tcbdb, synchttp_settings_dataname, BDBOWRITER | BDBOCREAT)) {
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
	if (synchttp_worker_pid < 0) {
		fprintf(stderr, "Error: %s:%d\n", __FILE__, __LINE__);
		exit(EXIT_FAILURE);
	}

	/* synchttp父进程内容 */
	if (synchttp_worker_pid > 0) {
		/* 处理父进程接收到的kill信号 */

		/* 忽略Broken Pipe信号 */
		signal(SIGPIPE, SIG_IGN);

		/* 处理kill信号 */
		signal(SIGINT, kill_signal_master);
		signal(SIGKILL, kill_signal_master);
		signal(SIGQUIT, kill_signal_master);
		signal(SIGTERM, kill_signal_master);
		signal(SIGHUP, kill_signal_master);

		/* 处理段错误信号 */
		signal(SIGSEGV, kill_signal_master);

		/* 如果子进程终止，则重新派生新的子进程 */
		while (1) {
			synchttp_worker_pid_wait = wait(NULL);
			if (synchttp_worker_pid_wait < 0) {
				continue;
			}
			usleep(100000);
			synchttp_worker_pid = fork();
			if (synchttp_worker_pid == 0) {
				break;
			}
		}
	}

	/*****************************************子进程处理************************************/
	/* 忽略Broken Pipe信号 */
	signal(SIGPIPE, SIG_IGN);

	/* 处理kill信号 */
	signal(SIGINT, kill_signal_worker);
	signal(SIGKILL, kill_signal_worker);
	signal(SIGQUIT, kill_signal_worker);
	signal(SIGTERM, kill_signal_worker);
	signal(SIGHUP, kill_signal_worker);

	/* 处理段错误信号 */
	signal(SIGSEGV, kill_signal_worker);

	/*创建消息队列监听进程*/
	pthread_t synchttp_dispatch_tid;
	pthread_create(&synchttp_dispatch_tid, NULL, (void *) synchttp_dispatch, NULL);

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
