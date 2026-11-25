CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0)
LIBS = $(shell pkg-config --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0)

all: servidor cliente

servidor: servidor.c
	$(CC) $(CFLAGS) servidor.c -o servidor $(LIBS)

cliente: cliente.c
	$(CC) $(CFLAGS) cliente.c -o cliente $(LIBS)

clean:
	rm -f servidor cliente

.PHONY: all clean
