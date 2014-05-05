.PHONY : all clean 

CC = gcc 
BINDIR = bin
LIBDIR = lib
CFLAGS = -g -Wall -fPIC
LDFLAGS = -llua -lpthread -ldl -lm #lua调用了标准数学库 -lm


all : \
	libnet.a \
	skynet

libnet.a : \
	net/socket_server.c
	$(CC) $(CFLAGS) -c $^ -Inet
	ar rcv $(LIBDIR)/$@ \
	socket_server.o
	ranlib $(LIBDIR)/$@
	rm *.o

skynet : \
	src/malloc_hook.c \
	src/skynet_env.c \
	src/skynet_handle.c \
	src/skynet_harbor.c \
	src/skynet_monitor.c \
	src/skynet_timer.c \
	src/skynet_module.c \
	src/skynet_mq.c \
	src/skynet_error.c \
	src/skynet_main.c \
	src/skynet_start.c \
	src/skynet_server.c \
	src/skynet_socket.c
	$(CC) $(CFLAGS) -c $^ -I./net
	$(CC) $(CFLAGS) -o $(BINDIR)/$@ \
	*.o lib/libnet.a -I./net $(LDFLAGS)
	rm *.o

clean :
	rm *.o lib/*.a bin/skynet
