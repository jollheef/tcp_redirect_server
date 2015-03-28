CFLAGS += -std=gnu11 -lpthread

CFLAGS += -Wall -Werror

all:
	gcc $(CFLAGS) tcpserver.c -o tcpserver

debug:
	gcc -o -g $(CFLAGS) -DDEBUG -DTRACE tcpserver.c -o tcpserver

valgrind: debug
	valgrind --show-leak-kinds=all --leak-check=full ./tcpserver

trace:
	gcc $(CFLAGS) -DTRACE tcpserver.c -o tcpserver	

run: all
	./tcpserver

clean:
	rm ./tcpserver
