CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -O2

COMMON = common.c net.c common.h net.h
PROGS  = server cook waiter door observer

all: $(PROGS)

# сервер — кухня ресторана
server: server.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ server.c common.c net.c

# клиенты
cook: cook.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ cook.c common.c net.c

waiter: waiter.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ waiter.c common.c net.c

door: door.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ door.c common.c net.c

observer: observer.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ observer.c common.c net.c

test: all
	./tests/run_tests.sh

clean:
	rm -f $(PROGS) *.log
	rm -rf logs tests/out

.PHONY: all test clean
