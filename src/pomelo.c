#include "pomelo.h"
#include "http.h"

#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <pthread.h>
#include <sys/stat.h>

/*
 * TCP SOCKET LAYER
 *
 */

const char coloredLogFOK[] = 	"\x1B[34m%8.3f\x1B[0m %s\t%s\n";
const char coloredLogFFAIL[] = 	"\x1B[31m%8.3f\x1B[0m %s\t%s\n";
const char logF[] = 	   		"%8.3f %s\t%s\n";

void httplog(FILE* out, int isOK, float msec, const char* file) {
	const char* format = logF;
	if (out == stdout) {
		format = isOK ? coloredLogFOK : coloredLogFFAIL;
	}
	int secFlag = msec > 1000;
	if (secFlag) {
		fprintf(out, format, msec * 1000, " s", file);
	} else {
		fprintf(out, format, msec, "ms", file);
	}
}

#define SAFE_FREE(ptr) { if ((ptr) != NULL) {free(ptr); ptr = NULL;} }

static char* servingDir;
static size_t servingDir_len;

static int is_dir(FILE* f) {
	struct stat s;
	int status = fstat(fileno(f), &s);
	if (status != 0) {
		return 0;
	}
	return S_ISDIR(s.st_mode);
}

static void add_index_html(char** sPtr) {
	size_t n = strlen(*sPtr);
	size_t new_size = n + 10;  // len of index.html

	*sPtr = realloc(*sPtr, new_size + 1);

	memcpy((*sPtr) + n, "index.html", 10);
	(*sPtr)[n + 10] = '\0';
}

static FILE* open_not_dir(const char* filename) {
	FILE* f = fopen(filename, "r");
	if (f == NULL) {
		return NULL;
	} else {
		if (is_dir(f)) {
			fclose(f);
			return NULL;
		}
	}
	return f;
}

// Add serving directory to the requested path.
static char* make_full_path(char* location) {
	size_t n = strlen(location);
	size_t final_len = servingDir_len + n;
	char* buffer = calloc(1, final_len + 1);
	memcpy(buffer, servingDir, servingDir_len);
	memcpy(buffer + servingDir_len, location, n);
	return buffer;
}

// Some typedefs to help out in Linux socket calls.
typedef struct sockaddr_in sockaddr_in;
typedef struct sockaddr sockaddr;

static const size_t DEFAULT_LINE_LEN = 64;

// Possible worker status values.
typedef enum worker_status_t {
	ready,		// Can be used
	working 	// Is handling a connection. Can't accept new connection.
} worker_status_t;

// HTTP server worker.
typedef struct Worker {
	sockaddr_in client_addr;	// Address of a client.
	FILE* sfile;				// FILE* representing socket connection.

	worker_status_t status;		// Current worker status.

	pthread_t thread_info;		// Thread info. It holds thread id and other things.

	char* lineBuffer;
	size_t line_buff_size;
	size_t line_length;

	char* req_line;
	char* req_method;
	char* req_path;
	char* req_full_path;
	char* req_version;
} Worker;

static void prep_worker(Worker* w) {
	w->req_line = NULL;
	w->req_method = NULL;
	w->req_path = NULL;
	w->req_full_path = NULL;
	w->req_version = NULL;
}

static void clean_worker(Worker* w) {
	SAFE_FREE(w->req_line);
	SAFE_FREE(w->req_method);
	SAFE_FREE(w->req_path);
	SAFE_FREE(w->req_full_path);
	SAFE_FREE(w->req_version);
}

// Append char c into Workers line buffer + expand it if needed.
static void add_char_to_line(Worker* w, char c) {
	if (w->line_length == w->line_buff_size) {
		w->line_buff_size *= 2;
		w->lineBuffer = realloc(w->lineBuffer, w->line_buff_size);
	}
	w->lineBuffer[w->line_length] = c;
	w->line_length++;
}

// return values:
// 	0 read empty line ending in <CR><LF>
// 	> 0 number of chars in line ending in <CR><LF>
// 	-1 EOF at the start of the read.
// 	-2 EOF while reading (before <CR><LF>) 
//	-3 <CR> but no <LF>
static int read_line(Worker* w) {
	w->line_length = 0;
	int c;
	int n = 0;
	while (1) {
		c = fgetc(w->sfile);
		if (c == EOF) {
			if (n == 0) {
				return -1;
			} else {
				return -2;
			}
		}
		if (c == CR) {
			c = fgetc(w->sfile);
			if (c == LF) {
				// <CR><LF> was read.
				return n;
			} else {
				return -3;
			}
		}
		add_char_to_line(w, (char)c);
		n++;
	}
	return n;
}

// Read first line, extract path and read + discard all the other information.
// No real need to examine it.
static int read_request(Worker* w) {
	int i = read_line(w);
	if (i <= 0) {
		return -1;
	}

	w->req_line = calloc(1, i + 1);
	memcpy(w->req_line, w->lineBuffer, i);

	int n = fill_http_method(w->req_line, i, &w->req_method);
	if (n < 0) {
		return -1;
	}
	char* workingLine = w->req_line + n + 1;

	int nFullPath = fill_http_full_path(workingLine, i - n - 1, &w->req_full_path);
	if (nFullPath < 0) {
		return -1;
	}

	int nVersion = fill_http_version(workingLine, i - n - 1 - nFullPath - 1, &w->req_version);
	if (nVersion < 0) {
		return -1;
	}

	while((i = read_line(w)) >= 0) {
		if (i == 0) {
			return 0;
		}
	}
	if (i == -1) {
		return 0;
	} else {
		return -1;
	}
}


// Reads http request, parses it and handles the request.
static void read_parse_handle(Worker* w) {
	clock_t start = clock();
	clock_t diff;

	int status = 0;

	int r = read_request(w);
	if (r != 0) {
		resp_bad_req(w->sfile);
	} else {
		extract_http_path(w->req_full_path, &w->req_path);
		char* absolutePath = make_full_path(w->req_path);
		FILE* f = open_not_dir(absolutePath);
		if (f == NULL) {
			add_index_html(&absolutePath);
			f = open_not_dir(absolutePath);
		}

		if (f == NULL) {
			resp_not_found(w->sfile);
		} else {
			resp_serve_file(w->sfile, f);
			status = 1;
		}

		free(absolutePath);
	}

	diff = clock() - start;
	float msec = ((float)diff * 1000) / CLOCKS_PER_SEC;
	if (w->req_line != NULL) {
		httplog(stdout, status, msec, w->req_path);
	}
	return;
}

// Prepares a new worker to accept connections.
static void init_worker(Worker* w) {
	w->sfile = NULL;
	w->status = ready;

	w->line_buff_size = DEFAULT_LINE_LEN;
	w->line_length = 0;
	w->lineBuffer = calloc(sizeof(char), w->line_buff_size);
}

typedef struct worker_pool {
	Worker* workers;
	size_t nr;
} worker_pool;

worker_pool* new_worker_pool(size_t n) {
	worker_pool* p = malloc(sizeof(worker_pool));
	p->workers = malloc(sizeof(Worker) * n);
	for (size_t i = 0; i < n; i++) {
		init_worker(&(p->workers[i]));
	}
	p->nr = n;
	return p;
}

static void delete_worker_pool(worker_pool* p) {
	for (size_t i = 0; i < p->nr; i++) {
		free(p->workers[i].lineBuffer);
		pthread_join(p->workers[i].thread_info, NULL);
	}
	free(p->workers);
	free(p);
}

// Blocking-while-pool-empty connction distributor.
static Worker* get_ready_worker(worker_pool* pool) {
	Worker* array = pool->workers;
	Worker* current = array;
	size_t n = pool->nr;
	size_t i = 0;
	while (current->status != ready) {
		i = (i + 1) % n;
		current = &(array[i]);
	}
	current->status = working;
	return current;
}

// Thread function that pthread_create() calls.
// Worker has it's socket file descriptor set.
static void* worker_thread(void* arg) {
	Worker* w = (Worker*)arg;
	prep_worker(w);

	read_parse_handle(w);

	fclose(w->sfile);
	w->sfile = NULL;

	clean_worker(w);

	w->status = ready;
	pthread_exit(NULL);
}

static int get_bind_listener(int port) {
	int listener = socket(AF_INET, SOCK_STREAM, 0);

	if (listener < 0) {
		return -1;		
	}

	int t = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(int));

	sockaddr_in serv_addr;

	memset(&serv_addr, 0x00, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	if (bind(listener, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		return -2;
	}
	return listener;
}

static int* globalListener;
worker_pool* globalPool;

int run_pomelo(int port, const char* dir) {
	size_t n_workers = 3;

	// Copy the serving directory.
	// If it ends in '/', we don't copy it. (Easyer concatenation)
	size_t dir_len = strlen(dir);
	if (dir[dir_len - 1] == '/') {
		dir_len -= 1;
	}
	servingDir = calloc(1, dir_len + 1);
	servingDir_len = dir_len;
	memcpy(servingDir, dir, dir_len);


	int listener = get_bind_listener(port);
	globalListener = &listener;
	if (listener < 0) {
		return listener;
	}

	listen(listener, 100);
	printf("Running on port %u\n", port);

	socklen_t clilen = sizeof(sockaddr);

	worker_pool* workers = new_worker_pool(n_workers);
	globalPool = workers;

	while (1) {
		Worker* w = get_ready_worker(workers);

		int fd = accept(listener, (sockaddr *)&(w->client_addr), &clilen);
		w->sfile = fdopen(fd, "r+");

		pthread_create(&(w->thread_info), NULL, worker_thread, (void*)w);
	}

	stop_pomelo();

	return 0;
}

void stop_pomelo() {
	if (globalListener != NULL) {
		close(*globalListener);
	}

	if (globalPool != NULL) {
		delete_worker_pool(globalPool);
	}

	if (servingDir != NULL) {
		free(servingDir);
	}
	exit(1);
}
