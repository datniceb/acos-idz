#!/bin/bash
# Тесты. Запуск: make test или ./tests/run_tests.sh
#
# Для каждого теста в tests/ лежит файл ИМЯ.in — ответы на вопросы сервера, по одному в строке
# (сценарий, «изменить параметры?», затем параметры в том порядке, в котором их спрашивает сервер).
# Скрипт запускает сервер и клиентов на одном компьютере (127.0.0.1, свой порт на каждый тест),
# вывод каждой программы сохраняет в tests/out/ИМЯ.ПРОГРАММА.out, а grep проверяет, что в нём
# есть нужные строки. После каждого теста проверяется, что вместе с сервером завершились
# и все клиенты.

cd "$(dirname "$0")/.." || exit 1
OUT=tests/out
rm -rf $OUT
mkdir -p $OUT
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

# start_server ТЕСТ — запустить сервер в фоне с ответами из tests/ТЕСТ.in
start_server() {
    T=$1
    PORT=$((20000 + RANDOM % 20000))
    ./server $PORT < tests/$T.in > $OUT/$T.server.out 2>&1 &
    server_pid=$!
    pids=()
    sleep 0.3
}

# client ИМЯ_ФАЙЛА ПРОГРАММА [аргументы] — запустить клиента в фоне
client() {
    local name=$1 prog=$2
    shift 2
    ./$prog 127.0.0.1 $PORT "$@" > $OUT/$T.$name.out 2>&1 &
    last_pid=$!
    pids+=($last_pid)
}

# staff ПОВАРОВ ОФИЦИАНТОВ — повара, официанты, наблюдатель и вход
staff() {
    for i in $(seq $1); do client cook$i cook; done
    for i in $(seq $2); do client waiter$i waiter; done
    client observer observer
    client door door 1
}

# finish СЕКУНДЫ — ждём, пока сервер закончит сам, а за ним — все клиенты (бонус:
# при завершении сервера клиенты завершаются сами)
finish() {
    for i in $(seq $(($1 * 10))); do
        kill -0 $server_pid 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 $server_pid 2>/dev/null; then
        echo "  ОШИБКА: сервер в тесте $T не завершился за $1 с"
        kill -9 $server_pid
        failed=$((failed + 1))
    fi
    for i in $(seq 30); do
        left=0
        for p in "${pids[@]}"; do kill -0 $p 2>/dev/null && left=1; done
        [ $left = 0 ] && break
        sleep 0.1
    done
    if [ $left = 1 ]; then
        echo "  ОШИБКА: после сервера в тесте $T остались клиенты"
        for p in "${pids[@]}"; do kill -9 $p 2>/dev/null; done
        failed=$((failed + 1))
    fi
    wait 2>/dev/null
}

# run ТЕСТ ПОВАРОВ ОФИЦИАНТОВ — обычный тест: запустить всех и ждать конца
run() {
    start_server $1
    staff $2 $3
    finish 60
}

echo "=== Обычная работа ==="

run 01_normal 4 3
check $OUT/01_normal.server.out "Пришло посетителей: 12" "Не обслужено к моменту остановки: 0" \
      "все посетители ушли — ресторан закрывается" "Нарушений инвариантов: 0"
check $OUT/01_normal.cook1.out "получил" "закончил" "ресторан закрылся — ухожу со смены"
check $OUT/01_normal.waiter1.out "принял заказ" "выдал заказ" "ресторан закрылся — ухожу со смены"
check $OUT/01_normal.door.out "получил свой заказ" "ресторан закрылся — вход закрыт"
check $OUT/01_normal.observer.out "Координатор: заказ #1 взят в работу" "Нарушений инвариантов: 0" \
      "ресторан закрылся — наблюдатель отключается"

run 02_full_queues 3 2
check $OUT/02_full_queues.server.out "очередь заказов кухни заполнена" "в зоне готовых блюд нет места" \
      "Пришло посетителей: 20" "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"
check $OUT/02_full_queues.waiter1.out "ждёт у раздачи"

run 03_no_equipment 3 2
check $OUT/03_no_equipment.server.out "нужно оборудование" "Отменено невыполнимых заказов: 10" \
      "Не обслужено к моменту остановки: 0"
check $OUT/03_no_equipment.door.out "ушёл без заказа"

run 04_small_zone 4 3
check $OUT/04_small_zone.server.out "блюд больше, чем мест в зоне готовых блюд" "Пришло посетителей: 30" \
      "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"

run 05_one_of_each 1 1
check $OUT/05_one_of_each.server.out "Пришло посетителей: 15" "Не обслужено к моменту остановки: 0" \
      "Нарушений инвариантов: 0"

run 06_many 6 5
check $OUT/06_many.server.out "Пришло посетителей: 150" "Не обслужено к моменту остановки: 0" \
      "Нарушений инвариантов: 0"

run 07_bad_input 4 3
check $OUT/07_bad_input.server.out "Ошибка: нужно целое число от 1 до 4" "Ошибка: нужно целое число от 0 до 200" \
      "Ошибка: нужно целое число от 1 до 5000" "Пришло посетителей: 12" "Не обслужено к моменту остановки: 0"

run 08_one_waiter 2 1
check $OUT/08_one_waiter.server.out "ждёт у раздачи" "Пришло посетителей: 10" \
      "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"

echo "=== Ctrl+C на сервере ==="

# один Ctrl+C в режиме без ограничения: вход закрывается, все, кто внутри, обслуживаются
start_server 09_interrupt_unlimited
staff 4 3
sleep 1
kill -INT $server_pid
finish 60
check $OUT/09_interrupt_unlimited.server.out "получен Ctrl+C — вход закрыт" \
      "все посетители ушли — ресторан закрывается" "Не обслужено к моменту остановки: 0"
check $OUT/09_interrupt_unlimited.door.out "вход закрыт — новых посетителей не впускаю"

# Ctrl+C два раза: немедленная остановка, клиенты получают BYE и завершаются
start_server 10_interrupt_twice
staff 3 2
sleep 1
kill -INT $server_pid
sleep 0.5
kill -INT $server_pid
finish 10
check $OUT/10_interrupt_twice.server.out "немедленная остановка" "моделирование остановлено" "=== Итоги ==="
check $OUT/10_interrupt_twice.cook1.out "ресторан закрыт немедленно — ухожу со смены"
check $OUT/10_interrupt_twice.door.out "ушёл, не дождавшись заказа"

# все посетители пришли сразу, вход уже закрыт — один Ctrl+C останавливает сразу
start_server 11_interrupt_closed
staff 4 3
sleep 1
kill -INT $server_pid
finish 10
check $OUT/11_interrupt_closed.server.out "вход закрыт: пришли все 12 посетителей" "немедленная остановка" "=== Итоги ==="

echo "=== Отключение и повторное подключение клиентов ==="

# один повар и один официант; повара, официанта, вход и наблюдателя по ходу работы
# отключают (kill) и запускают снова — ресторан работает дальше и обслуживает всех
start_server 12_reconnect
client cook1 cook;         cook1=$last_pid
client waiter1 waiter;     waiter1=$last_pid
client observer1 observer; observer1=$last_pid
client door1 door 1;       door1=$last_pid
sleep 0.6
kill $cook1 $door1 $observer1
sleep 0.4
client cook2 cook
client door2 door 2
client observer2 observer
sleep 0.5
kill $waiter1
sleep 0.3
client waiter2 waiter
finish 60
check $OUT/12_reconnect.server.out "Повар 1: .*отключился" "Вход: отключился" "Вход: подключён снова" \
      "наблюдатель отключился" "Официант 1: .*отключился" "Пришло посетителей: 12" \
      "Не обслужено к моменту остановки: 0" "Нарушений инвариантов: 0"
check $OUT/12_reconnect.cook2.out "Повар 1: пришёл на смену" "ресторан закрылся — ухожу со смены"
check $OUT/12_reconnect.observer2.out "сейчас пришло посетителей" "ресторан закрылся — наблюдатель отключается"

echo
if [ $failed -ne 0 ]; then
    echo "Не пройдено проверок: $failed"
    exit 1
fi
echo "Все тесты пройдены"
