#!/bin/bash
# Тесты для обеих программ. Запуск: make test или ./tests/run_tests.sh
#
# Для каждого теста в tests/ лежит файл ИМЯ.in — ответы на вопросы программы, по одному
# в строке (сценарий, «изменить параметры?», затем параметры в том порядке, в котором
# их спрашивает программа). Вывод сохраняется в tests/ИМЯ.ПРОГРАММА.out, а grep
# проверяет, что в нём есть нужные строки. Порядок событий в многопоточной программе
# от запуска к запуску разный, поэтому проверяются только итоги и ключевые события.

cd "$(dirname "$0")/.." || exit 1
failed=0

# check ФАЙЛ СТРОКА... — в файле должны быть все перечисленные строки
check() {
    local file=$1
    shift
    for s in "$@"; do
        if ! grep -q -- "$s" "$file"; then
            echo "  ОШИБКА: в $file нет строки «$s»"
            failed=$((failed + 1))
            return
        fi
    done
    echo "  OK  $file"
}

# wait_end PID СЕКУНДЫ — ждём, пока программа закончится сама (не дольше заданного)
wait_end() {
    for i in $(seq $(($2 * 10))); do
        if ! kill -0 $1 2>/dev/null; then
            wait $1
            return 0
        fi
        sleep 0.1
    done
    kill -9 $1
    echo "  ОШИБКА: программа не завершилась за $2 с"
    failed=$((failed + 1))
    return 1
}

for P in restaurant restaurant_cv; do
    echo "=== $P ==="

    # run ТЕСТ — запустить программу с ответами из tests/ТЕСТ.in
    run() {
        timeout 120 ./$P < tests/$1.in > tests/$1.$P.out 2>&1
    }

    run 01_normal
    check tests/01_normal.$P.out "Пришло посетителей: 12" "Не обслужено к моменту остановки: 0" \
          "все посетители ушли — ресторан закрывается" "Нарушений инвариантов: 0"

    run 02_full_queues
    check tests/02_full_queues.$P.out "очередь заказов кухни заполнена" "в зоне готовых блюд нет места" \
          "Пришло посетителей: 20" "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"

    run 03_no_equipment
    check tests/03_no_equipment.$P.out "нужно оборудование" "Отменено невыполнимых заказов: 10" \
          "Не обслужено к моменту остановки: 0"

    run 04_no_cooks
    check tests/04_no_cooks.$P.out "на кухне нет поваров" "Отменено невыполнимых заказов: 8" \
          "Не обслужено к моменту остановки: 0"

    run 05_small_zone
    check tests/05_small_zone.$P.out "блюд больше, чем мест в зоне готовых блюд" "Пришло посетителей: 30" \
          "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"

    run 06_one_of_each
    check tests/06_one_of_each.$P.out "Пришло посетителей: 15" "Не обслужено к моменту остановки: 0" \
          "Нарушений инвариантов: 0"

    run 07_many
    check tests/07_many.$P.out "Пришло посетителей: 1000" "Не обслужено к моменту остановки: 0" \
          "Нарушений инвариантов: 0"

    run 08_bad_input
    check tests/08_bad_input.$P.out "Ошибка: нужно целое число от 1 до 4" "Ошибка: нужно целое число от 0 до 1000" \
          "Ошибка: нужно целое число от 1 до 5000" "Пришло посетителей: 12" "Не обслужено к моменту остановки: 0"

    run 12_one_waiter
    check tests/12_one_waiter.$P.out "ждёт у раздачи" "Пришло посетителей: 10" \
          "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"

    # Ctrl+C один раз в режиме без ограничения: вход закрывается, все, кто внутри,
    # обслуживаются, и программа заканчивается сама
    ./$P < tests/09_interrupt_unlimited.in > tests/09_interrupt_unlimited.$P.out 2>&1 &
    pid=$!
    sleep 1
    kill -INT $pid
    if wait_end $pid 60; then
        check tests/09_interrupt_unlimited.$P.out "получен Ctrl+C — вход закрыт" \
              "все посетители ушли — ресторан закрывается" "Не обслужено к моменту остановки: 0"
    fi

    # Ctrl+C два раза: немедленная остановка, итоги всё равно печатаются
    ./$P < tests/10_interrupt_twice.in > tests/10_interrupt_twice.$P.out 2>&1 &
    pid=$!
    sleep 1
    kill -INT $pid
    sleep 0.5
    kill -INT $pid
    if wait_end $pid 10; then
        check tests/10_interrupt_twice.$P.out "немедленная остановка" "моделирование остановлено" "=== Итоги ==="
    fi

    # все посетители пришли сразу, вход уже закрыт — один Ctrl+C останавливает сразу
    ./$P < tests/11_interrupt_closed.in > tests/11_interrupt_closed.$P.out 2>&1 &
    pid=$!
    sleep 1
    kill -INT $pid
    if wait_end $pid 10; then
        check tests/11_interrupt_closed.$P.out "вход закрыт: пришли все 12 посетителей" \
              "немедленная остановка" "=== Итоги ==="
    fi
done

echo
if [ $failed -ne 0 ]; then
    echo "Не пройдено проверок: $failed"
    exit 1
fi
echo "Все тесты пройдены"
