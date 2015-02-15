#ifndef _HTTP_H_
#define _HTTP_H

#include <stdlib.h>
#include <stdio.h>

#define CR 0x0D
#define LF 0x0A

char* get_start_path(char* line, size_t max_len, size_t* n);
int fill_http_method(char* line, int lineLen, char** methodPtr);
int fill_http_full_path(char* line, int lineLen, char** fullPathPtr);
int fill_http_version(char* line, int lineLen, char** versionPtr);
void extract_http_path(char* fullPath, char** destPtr);

void resp_serve_file(FILE* client, FILE* input);

void resp_not_found(FILE* client);
void resp_bad_req(FILE* client);
void resp_internal_error(FILE* client);

#endif
