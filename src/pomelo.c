#include "pomelo.h"

#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <pthread.h>
#include <sys/stat.h>

/*
 * TCP SOCKET LAYER
 *
 */

#define CR 0x0D
#define LF 0x0A

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
	int add_slash = 0;
	if ((*sPtr)[n-1] != '/') {
		new_size += 1;
		add_slash = 1;
	}
	*sPtr = realloc(*sPtr, new_size + 1);
	if (add_slash) {
		(*sPtr)[n] = '/';
		n++;
	}
	memcpy((*sPtr) + n, "index.html", 10);
	(*sPtr)[n + 10] = '\0';
}

// Add serving directory to the requested path.
static char* make_full_path(char* location, size_t n) {
	size_t final_len = servingDir_len + n;
	char* buffer = calloc(1, final_len + 1);
	memcpy(buffer, servingDir, servingDir_len);
	memcpy(buffer + servingDir_len, location, n);
	return buffer;
}

// Write <CR><LF> to file
static void write_cr_lf(FILE* f) {
	fputc(CR, f);
	fputc(LF, f);
}

// Write 404 NOT FOUND Response to file.
static void write_404(FILE* f) {
	fputs("HTTP/1.1 ", f);
	fputs("404 Not Found", f);
	write_cr_lf(f);
	fputs("Connection: close", f);
	write_cr_lf(f);
	write_cr_lf(f);
	fputs("Not Found", f);
	write_cr_lf(f);
}

// Write 200 OK header to file
static void write_200(FILE* f) {
	fputs("HTTP/1.1 ", f);
	fputs("200 OK", f);
	write_cr_lf(f);
	fputs("Connection: close", f);
	write_cr_lf(f);
	write_cr_lf(f);
}

// Write file as a responses body
static void send_file(FILE* socketFile, FILE* f) {
	size_t size = 128;
	char buffer[size];
	while (! feof(f)) {
		size_t n = fread(buffer, 1, size, f);
		fwrite(buffer, 1, n, socketFile);
	}
}

// Check if file exsists, add index.html if it's a directory
// and try to serve it.
// Writes 200 + body if file is found (returns 0),
// otherwise returns -1 without writing anything.
static int write_file(FILE* f, char** namePtr) {
	FILE* in = fopen(*namePtr, "r");
	if (in == NULL) {
		return -1;
	}
	if (is_dir(in)) {
		fclose(in);
		add_index_html(namePtr);
		in = fopen(*namePtr, "r");
		if (in == NULL) {
			return -1;
		}
		if (is_dir(in)) {
			fclose(in);
			return -1;
		}
	}
	if (in == NULL) {
		return -1;
	}
	write_200(f);
	send_file(f, in);
	fclose(in);
	return 0;
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

	char* req_path;			
} Worker;

// Add cleanup work if needed.
static void reset_worker(Worker* w) {
	(void)(w);
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

// Find the start and length of path in "GET /index.html HTTP/1.1" string
static char* get_start_path(char* line, size_t max_len, size_t* n) {
	*n = 0;
	size_t start = 0;
	while ((line[start] != ' ') && (start <= max_len)) {
		start++;
	}
	if (start >= max_len) {
		return NULL;
	}
	char* p = line + start + 1; // First char after space
	max_len = max_len - (start + 1);

	while ((p[*n] != ' ') && (*n <= max_len)) {
		(*n)++;
	}
	if (*n >= max_len) {
		return NULL;
	}
	// p[*n] is a space => n - lengh of the path.
	return p;
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
	size_t n;
	char* start_path = get_start_path(w->lineBuffer, (size_t)i, &n);
	w->req_path = make_full_path(start_path, n);

	fwrite(w->lineBuffer, 1, i, stdout);
	fputc('\n', stdout);

	while((i = read_line(w)) >= 0) {
		if (i == 0) {
			return 0;
		}
	}
	if (i == -1) {
		return 0;
	} else {
		free(w->req_path);
		return -1;
	}
}

// Reads http request, parses it and handles the request.
static void read_parse_handle(Worker* w) {
	int r = read_request(w);
	if (r == 0) {
		r = write_file(w->sfile, &(w->req_path));
		if (r != 0) {
			printf("Not Found\n");
			write_404(w->sfile);
		} else {
			printf("OK\n");
		}
		free(w->req_path);
	} else {
		printf("Not Found\n");
		write_404(w->sfile);
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
	reset_worker(w);

	read_parse_handle(w);

	fclose(w->sfile);
	w->sfile = NULL;

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
