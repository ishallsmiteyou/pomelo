cc := gcc
link_flags := -pthread
flags := -std=gnu11 -Wall -Wextra
opt := -Os

cc := $(cc) $(opt) $(flags)

.PHONY: build
build: dir bin/pomelo


bin/pomelo: src/main.c src/pomelo.c src/http.c
	$(cc) $^ -o $@ $(link_flags)

.PHONY: dir
dir:
	- @ mkdir -p pkg/pomelo bin

.PHONY: clean
clean:
	rm -rf bin/* pkg/*

