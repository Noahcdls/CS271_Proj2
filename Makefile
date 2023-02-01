CC=gcc

client: client.o
	$(CC) client.o -o client

client.o: client.c
	$(CC) -c client.c

clean:
	rm -r *.o