#include <stdio.h>
#include <stdint.h>

#include "pomelo.h"

#include <signal.h>

void sig_handler(int n) {
	(void)(n);
	stop_pomelo();
}

int main(int argc, char** argv) {
	int port = 8000;
	char* dir;
	if (argc < 2) {
		dir = "/var/www";
	} else {
		dir = argv[1];
	}
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	int r = run_pomelo(port, dir);
	if (r != 0) {
		printf("FAILED WITH CODE %d\n", r);
		return 1;
	}
	return 0;
}
