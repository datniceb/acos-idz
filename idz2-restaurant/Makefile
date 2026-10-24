CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -O2 -pthread
COMMON = config.c util.c

all: restaurant restaurant_cv

# программа 1: мьютекс и семафоры
restaurant: restaurant_sem.c $(COMMON) restaurant.h
	$(CC) $(CFLAGS) -o restaurant restaurant_sem.c $(COMMON)

# программа 2: мьютекс и условные переменные
restaurant_cv: restaurant_cv.c $(COMMON) restaurant.h
	$(CC) $(CFLAGS) -o restaurant_cv restaurant_cv.c $(COMMON)

# тесты для обеих программ
test: all
	./tests/run_tests.sh

clean:
	rm -f restaurant restaurant_cv *.log tests/*.out

.PHONY: all test clean
