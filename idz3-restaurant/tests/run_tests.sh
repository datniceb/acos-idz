#!/bin/bash

cd "$(dirname "$0")/.." || exit 1
failed=0

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

staff() {
    case $1 in
        02_full_queues|03_no_equipment|10_interrupt_twice) echo "3 2" ;;
        04_no_cooks) echo "0 2" ;;
        06_one_of_each) echo "1 1" ;;
        07_many) echo "6 5" ;;
        12_one_waiter) echo "2 1" ;;
        *) echo "4 3" ;;
    esac
}

start() {
    local out=tests/$1.$P.out
    case $P in
        fork_named|fork_unnamed) ./$P < tests/$1.in > $out 2>&1 & ;;
        shm) ./run_shm.sh $(staff $1) < tests/$1.in > $out 2>&1 & ;;
    esac
    pid=$!
}

# ctrl_c — «нажать Ctrl+C»: в программах 1 и 2 сигнал получает родитель, в программе 3 — кухня
ctrl_c() {
    case $P in
        fork_named|fork_unnamed) kill -INT $pid ;;
        shm) pkill -INT -x shm_kitchen ;;
    esac
}

leftovers() {
    local left
    left=$(ls /dev/shm | grep -E '^(sem\.)?r[1-3]_')
    if [ -n "$left" ]; then
        echo "  ОШИБКА: после $P остались объекты:" $left
        failed=$((failed + 1))
    fi
}

finish() {
    for i in $(seq $(($1 * 10))); do
        if ! kill -0 $pid 2>/dev/null; then
            wait $pid
            leftovers
            return 0
        fi
        sleep 0.1
    done
    echo "  ОШИБКА: $P не завершилась за $1 с"
    pkill -9 -f run_shm.sh
    for prog in fork_named fork_unnamed shm_kitchen shm_cook shm_waiter shm_door; do
        pkill -9 -x $prog
    done
    failed=$((failed + 1))
    return 1
}

run() {
    start $1
    finish 120
}

for P in fork_named fork_unnamed shm; do
    echo "=== $P ==="

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
    start 09_interrupt_unlimited
    sleep 1
    ctrl_c
    if finish 60; then
        check tests/09_interrupt_unlimited.$P.out "получен Ctrl+C — вход закрыт" \
              "все посетители ушли — ресторан закрывается" "Не обслужено к моменту остановки: 0"
    fi

    # Ctrl+C два раза: немедленная остановка, итоги всё равно печатаются
    start 10_interrupt_twice
    sleep 1
    ctrl_c
    sleep 0.5
    ctrl_c
    if finish 10; then
        check tests/10_interrupt_twice.$P.out "немедленная остановка" "моделирование остановлено" "=== Итоги ==="
    fi

    # все посетители пришли сразу, вход уже закрыт — один Ctrl+C останавливает сразу
    start 11_interrupt_closed
    sleep 1
    ctrl_c
    if finish 10; then
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
