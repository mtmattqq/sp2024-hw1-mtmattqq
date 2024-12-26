all: server.c server.h
	gcc server.c -D WRITE_SERVER -o write_server
	gcc server.c -D READ_SERVER -o read_server
debug: server.c server.h
	gcc server.c -D WRITE_SERVER -D DEBUG -o write_server
	gcc server.c -D READ_SERVER -D DEBUG -o read_server
clean:
	rm -f *.o
	rm -f write_server read_server