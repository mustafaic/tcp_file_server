CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread

.PHONY: all clean

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client
	rm -f server.log
	rm -rf storage
