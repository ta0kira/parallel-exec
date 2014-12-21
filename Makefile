INSTALL=/usr/local/bin

.PHONY: default install clean

default: parallel-exec

parallel-exec: parallel-exec.c Makefile
	cc -std=gnu99 -Wall -pedantic -g -O2 -DBUFFER_PAGES=128 parallel-exec.c -o parallel-exec

install: $(INSTALL)/parallel-exec

$(INSTALL)/parallel-exec: parallel-exec
	cp -v parallel-exec $(INSTALL)

clean:
	rm -fv parallel-exec *~
