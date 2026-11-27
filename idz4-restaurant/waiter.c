/*
 * Клиент «официант». Получает от сервера задания: принять заказ у посетителя или отнести
 * собранный заказ. Выполняет (ждёт нужное число модельных минут) и сообщает серверу.
 * Официантов можно запускать и отключать в любой момент.
 *
 * Запуск: ./waiter IP ПОРТ
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

int main(int argc, char **argv)
{
    if (argc != 3 || parse_port(argv[2]) < 0) {
        write_str(STDERR_FILENO, "Запуск: ./waiter IP ПОРТ   (например ./waiter 127.0.0.1 5000)\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    static Reader r;
    int id = say_hello(&r, argv[1], parse_port(argv[2]), "HELLO WAITER");
    if (id < 0) return 1;
    log_event(WHO_WAITER, id, "пришёл на смену (подключился к серверу %s:%s)", argv[1], argv[2]);

    char line[LINE_MAX_LEN], list[400];
    while (recv_line(&r, line, sizeof(line)) == 0) {
        int v, t, pos;
        Order o;
        if (sscanf(line, "TAKE %d %d %n", &v, &t, &pos) == 2 && order_from_text(line + pos, &o) == 0) {
            /* TAKE посетитель минуты заказ — подойти к посетителю и принять заказ */
            log_event(WHO_WAITER, id, "подошёл к посетителю %d и принимает заказ", v + 1);
            sleep_minutes(t);
            dishes_text(&o, list, sizeof(list));
            log_event(WHO_WAITER, id, "принял заказ #%d%s: %s", v + 1, o.vip ? " (VIP)" : "", list);
            send_line(r.fd, "TAKEN");
        } else if (sscanf(line, "HOLD %d", &v) == 1) {
            log_event(WHO_WAITER, id, "очередь заказов кухни заполнена — ждёт у раздачи с заказом #%d", v + 1);
        } else if (sscanf(line, "PASSED %d", &v) == 1) {
            log_event(WHO_WAITER, id, "передал заказ #%d на кухню", v + 1);
        } else if (sscanf(line, "DELIVER %d %d", &v, &t) == 2) {
            /* DELIVER заказ минуты — отнести собранный заказ посетителю */
            log_event(WHO_WAITER, id, "забрал заказ #%d из зоны готовых блюд и несёт посетителю %d", v + 1, v + 1);
            sleep_minutes(t);
            log_event(WHO_WAITER, id, "выдал заказ #%d посетителю %d", v + 1, v + 1);
            send_line(r.fd, "DELIVERED");
        } else if (strncmp(line, "BYE", 3) == 0) {
            log_event(WHO_WAITER, id, "сервер: %s — ухожу со смены", line + 4);
            close(r.fd);
            return 0;
        }
    }
    log_event(WHO_WAITER, id, "сервер отключился — ухожу со смены");
    close(r.fd);
    return 0;
}
