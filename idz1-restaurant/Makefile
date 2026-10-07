CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -O2
SRC    = main.c config.c events.c hall.c kitchen.c util.c

restaurant: $(SRC) restaurant.h
	$(CC) $(CFLAGS) -o restaurant $(SRC)

# Тесты: программа запускается с вводом из tests/*.in,
# grep проверяет, что в выводе есть нужная строка.
# Если строки нет, make останавливается на этом тесте.
test: restaurant
	./restaurant < tests/01_normal.in > tests/01_normal.out
	grep "Пришло посетителей: 12" tests/01_normal.out
	grep "все посетители обслужены" tests/01_normal.out

	./restaurant < tests/02_full_queues.in > tests/02_full_queues.out
	grep -m1 "очередь заказов кухни заполнена" tests/02_full_queues.out
	grep "Не обслужено к моменту остановки: 0" tests/02_full_queues.out

	./restaurant < tests/03_no_equipment.in > tests/03_no_equipment.out
	grep "Отменено невыполнимых заказов: 10" tests/03_no_equipment.out

	./restaurant < tests/04_no_cooks.in > tests/04_no_cooks.out
	grep "Отменено невыполнимых заказов: 8" tests/04_no_cooks.out

	./restaurant < tests/05_small_zone.in > tests/05_small_zone.out
	grep -m1 "блюд больше, чем мест" tests/05_small_zone.out

	./restaurant < tests/06_one_of_each.in > tests/06_one_of_each.out
	grep "Не обслужено к моменту остановки: 0" tests/06_one_of_each.out

	./restaurant < tests/07_many.in > tests/07_many.out
	grep "Пришло посетителей: 1000" tests/07_many.out

	./restaurant < tests/08_bad_input.in > tests/08_bad_input.out
	grep -m1 "Ошибка: нужно целое число" tests/08_bad_input.out
	grep "Пришло посетителей: 12" tests/08_bad_input.out

	./restaurant < tests/09_interrupt_unlimited.in > tests/09_interrupt_unlimited.out & sleep 1; kill -INT $$!; wait
	grep "получен сигнал прерывания" tests/09_interrupt_unlimited.out

	./restaurant < tests/10_interrupt_scenario.in > tests/10_interrupt_scenario.out & sleep 1; kill -INT $$!; wait
	grep "получен сигнал прерывания" tests/10_interrupt_scenario.out

	@echo "Все тесты пройдены"

clean:
	rm -f restaurant tests/*.out

.PHONY: test clean
