#include "http.h"

#include <string.h>
#include <stdlib.h>

int fill_http_method(char* line, int lineLen, char** methodPtr) {
	if (lineLen <= 0) {
		return -1;
	}
	int n = 0;
	while (n < lineLen) {
		if (line[n] == ' ') {
			break;
		} else {
			n++;
		}
	}

	if (n == lineLen) {
		return -1;
	}

	*methodPtr = calloc(1, n + 1);
	memcpy(*methodPtr, line, n);
	return n;
}

int fill_http_full_path(char* line, int lineLen, char** fullPathPtr) {
	return fill_http_method(line, lineLen, fullPathPtr);
}

int fill_http_version(char* line, int lineLen, char** versionPtr) {
	if (lineLen < 0) {
		return -1;
	}
	*versionPtr = calloc(1, lineLen + 1);
	memcpy(*versionPtr, line, lineLen);
	return lineLen;
}

void extract_http_path(char* fullPath, char** destPtr) {
	int n = 0;
	int end = 0;
	while ((fullPath[n] != '\0') && (! end) ) {
		switch (fullPath[n]) {
			case '?':
				end = 1;
				break;
			case '#':
				end = 1;
				break;
			default:
				n++;
				break;
		}
	}
	*destPtr = calloc(1, n + 1);
	memcpy(*destPtr, fullPath, n);
}

// Write <CR><LF> to file
static inline void write_cr_lf(FILE* f) {
	fputc(CR, f);
	fputc(LF, f);
}

void write_resp_line(const char* str, FILE* f) {
	fputs(str, f);
	write_cr_lf(f);
}

// Write file as a responses body
static void send_file(FILE* socketFile, FILE* f) {
	size_t size = 256;
	char buffer[size];
	while (! feof(f)) {
		size_t n = fread(buffer, 1, size, f);
		fwrite(buffer, 1, n, socketFile);
	}
}

void resp_serve_file(FILE* client, FILE* input) {
	write_resp_line("HTTP/1.1 200 OK", client);
	write_resp_line("Connection: close", client);
	fprintf(client, "Cache-Control: max-age=%d", 60 * 5); // Cache for 5 minutes.
	write_cr_lf(client);
	write_cr_lf(client);
	send_file(client, input);
}

void resp_not_found(FILE* client) {
	write_resp_line("HTTP/1.1 404 Not Found", client);
	write_resp_line("Connection: close", client);
	write_cr_lf(client);
	write_resp_line("Not found", client);
}

void resp_bad_req(FILE* client) {
	write_resp_line("HTTP/1.1 400 Bad Request", client);
	write_resp_line("Connection: close", client);
	write_cr_lf(client);
	write_resp_line("Bad request", client);
}
void resp_internal_error(FILE* client) {
	write_resp_line("HTTP/1.1 500 Internal Server Error", client);
	write_resp_line("Connection: close", client);
	write_cr_lf(client);
	write_resp_line("Internal server error", client);
}

