.PHONY : all clean 

CC = gcc 
CFLAGS = -g -Wall -fPIC
LDFLAGS = -llua -lpthread -ldl -lm #lua调用了标准数学库 -lm


all : \
	libnet.a \
	skynet

libnet.a : \
	skynet-src/socket_server.c
	$(CC) $(CFLAGS) -c $^ -Inet
	ar rcv $@ \
	socket_server.o
	ranlib $@
	rm *.o

skynet : \
	skynet-src/malloc_hook.c \
	skynet-src/skynet_env.c \
	skynet-src/skynet_handle.c \
	skynet-src/skynet_harbor.c \
	skynet-src/skynet_monitor.c \
	skynet-src/skynet_timer.c \
	skynet-src/skynet_module.c \
	skynet-src/skynet_mq.c \
	skynet-src/skynet_error.c \
	skynet-src/skynet_main.c \
	skynet-src/skynet_start.c \
	skynet-src/skynet_server.c \
	skynet-src/skynet_socket.c
	$(CC) $(CFLAGS) -c $^ -I./net
	$(CC) $(CFLAGS) -o $@ \
	*.o lib/libnet.a -I./net $(LDFLAGS)
	rm *.o

clean :
	rm *.a skynet
