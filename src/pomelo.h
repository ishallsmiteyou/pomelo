#ifndef _POMELO_H_
#define _POMELO_H_

#include <stdio.h>

// Run server on port <port>, serving absolute directory <dir>, with <nrWorkers> workers and write log to <log>.
int run_pomelo(int port, const char* dir, int nrWorkers, FILE* log);

void stop_pomelo();

#endif // _POMELO_H
