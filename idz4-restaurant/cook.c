/*
 * Клиент «повар». Подключается к серверу, получает операции, «готовит» (ждёт нужное
 * число модельных минут) и сообщает, что закончил. Видит только то, что ему прислали.
 * Поваров можно запускать сколько угодно раз, в любой момент, и отключать (Ctrl+C):
 * незаконченная операция вернётся в очередь, её доделает другой повар.
 *
 * Запуск: ./cook IP ПОРТ
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
        write_str(STDERR_FILENO, "Запуск: ./cook IP ПОРТ   (например ./cook 127.0.0.1 5000)\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    static Reader r;
    int id = say_hello(&r, argv[1], parse_port(argv[2]), "HELLO COOK");
    if (id < 0) return 1;
    log_event(WHO_COOK, id, "пришёл на смену (подключился к серверу %s:%s)", argv[1], argv[2]);

    char line[LINE_MAX_LEN];
    while (recv_line(&r, line, sizeof(line)) == 0) {
        int o, d, m, s, u;
        /* COOK заказ блюдо блюдо_меню шаг единица_оборудования */
        if (sscanf(line, "COOK %d %d %d %d %d", &o, &d, &m, &s, &u) == 5 && m >= 0 && m < MENU_SIZE && s >= 0 && s < MENU[m].nops) {
            Operation op = MENU[m].ops[s];
            log_event(WHO_COOK, id, "получил %s №%d и готовит «%s» (заказ #%d, блюдо %d: %s), шаг %d/%d — %d мин",
                      EQ_ACC[op.eq], u + 1, op.name, o + 1, d + 1, MENU[m].name, s + 1, MENU[m].nops, op.time);
            sleep_minutes(op.time);
            log_event(WHO_COOK, id, "закончил «%s» (заказ #%d) и освободил %s №%d", op.name, o + 1, EQ_ACC[op.eq], u + 1);
            send_line(r.fd, "DONE");
        } else if (strncmp(line, "BYE", 3) == 0) {
            log_event(WHO_COOK, id, "сервер: %s — ухожу со смены", line + 4);
            close(r.fd);
            return 0;
        }
    }
    log_event(WHO_COOK, id, "сервер отключился — ухожу со смены");
    close(r.fd);
    return 0;
}
