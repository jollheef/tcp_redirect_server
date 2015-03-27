CFLAGS += -std=c99 -lpthread

all:
	gcc $(CFLAGS) tcpserver.c -o tcpserver

debug:
	gcc -g $(CFLAGS) -DDEBUG tcpserver.c -o tcpserver

trace:
	gcc $(CFLAGS) -DTRACE tcpserver.c -o tcpserver	

run: all
	./tcpserver

clean:
	rm ./tcpserver
