CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -O2
LIBS   = -pthread -lrt

COMMON = common.c
SHM    = shm.c shm_roles.c
PROGS  = fork_named fork_unnamed shm_kitchen shm_cook shm_waiter shm_door

all: $(PROGS)

# программа 1: fork, именованные семафоры, разделяемая память
fork_named: fork_named.c fork_main.c $(SHM) $(COMMON) common.h shm.h
	$(CC) $(CFLAGS) -o $@ fork_named.c fork_main.c $(SHM) $(COMMON) $(LIBS)

# программа 2: fork, неименованные семафоры в разделяемой памяти
fork_unnamed: fork_unnamed.c fork_main.c $(SHM) $(COMMON) common.h shm.h
	$(CC) $(CFLAGS) -o $@ fork_unnamed.c fork_main.c $(SHM) $(COMMON) $(LIBS)

# программа 3: независимые процессы, именованные семафоры, разделяемая память
shm_kitchen: shm_kitchen.c $(SHM) $(COMMON) common.h shm.h
	$(CC) $(CFLAGS) -o $@ shm_kitchen.c $(SHM) $(COMMON) $(LIBS)

shm_cook: shm_cook.c $(SHM) $(COMMON) common.h shm.h
	$(CC) $(CFLAGS) -o $@ shm_cook.c $(SHM) $(COMMON) $(LIBS)

shm_waiter: shm_waiter.c $(SHM) $(COMMON) common.h shm.h
	$(CC) $(CFLAGS) -o $@ shm_waiter.c $(SHM) $(COMMON) $(LIBS)

shm_door: shm_door.c $(SHM) $(COMMON) common.h shm.h
	$(CC) $(CFLAGS) -o $@ shm_door.c $(SHM) $(COMMON) $(LIBS)

test: all
	./tests/run_tests.sh

clean:
	rm -f $(PROGS) *.log tests/*.out

.PHONY: all test clean
