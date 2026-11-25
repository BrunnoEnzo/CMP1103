CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0)
LIBS = $(shell pkg-config --libs gtk+-3.0 gstreamer-1.0)

all: servidor cliente streaming_audio

servidor: servidor.c
	$(CC) $(CFLAGS) servidor.c -o servidor $(LIBS)

cliente: cliente.c
	$(CC) $(CFLAGS) cliente.c -o cliente $(LIBS)

streaming_audio: streaming_audio.c
	$(CC) -Wall -Wextra -O2 $(shell pkg-config --cflags gstreamer-1.0) streaming_audio.c -o streaming_audio $(shell pkg-config --libs gstreamer-1.0)

clean:
	rm -f servidor cliente streaming_audio

.PHONY: all clean
