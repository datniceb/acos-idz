/*
 * Клиент «вход». Впускает посетителей со случайными интервалами. Каждый посетитель —
 * отдельный процесс (fork) со своим подключением к серверу: он отдаёт заказ, ждёт
 * официанта и свой заказ и уходит. Сколько посетителей и как часто они приходят,
 * вход узнаёт у сервера. Если вход отключить и запустить снова, посетители
 * продолжат приходить с того места, где остановились.
 *
 * Запуск: ./door IP ПОРТ [зерно]   (зерно генератора случайных чисел — чтобы повторить запуск)
 */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

static const char *server_ip;
static int server_port;

/* Процесс посетителя: подключиться со своим заказом и ждать, что скажет ресторан */
static void visitor(const Order *o)
{
    static Reader r;
    char hello[200], order_text[100], list[400], line[LINE_MAX_LEN], reason[300];
    int w;
    double t;

    signal(SIGINT, SIG_IGN);        /* Ctrl+C в окне входа не выгоняет тех, кто уже внутри */
    order_to_text(o, order_text, sizeof(order_text));
    snprintf(hello, sizeof(hello), "HELLO VISITOR %s", order_text);
    int v = say_hello(&r, server_ip, server_port, hello);
    if (v < 0) return;
    dishes_text(o, list, sizeof(list));
    log_event(WHO_VISITOR, v, "пришёл в ресторан%s, хочет заказать: %s", o->vip ? " (VIP)" : "", list);

    while (recv_line(&r, line, sizeof(line)) == 0) {
        if (sscanf(line, "WAITER %d", &w) == 1) {
            log_event(WHO_VISITOR, v, "делает заказ официанту %d", w + 1);
        } else if (strcmp(line, "WAITER_LEFT") == 0) {
            log_event(WHO_VISITOR, v, "официант ушёл, не приняв заказ — снова ждёт официанта");
        } else if (sscanf(line, "SERVED %d %lf", &w, &t) == 2) {
            log_event(WHO_VISITOR, v, "получил свой заказ от официанта %d и ушёл, провёл в ресторане %.1f мин", w + 1, t);
            break;
        } else if (sscanf(line, "CANCELLED %299[^\n]", reason) == 1) {
            log_event(WHO_VISITOR, v, "заказ отменён (%s) — ушёл без заказа", reason);
            break;
        } else if (strncmp(line, "BYE", 3) == 0) {
            log_event(WHO_VISITOR, v, "сервер: %s — ушёл, не дождавшись заказа", line + 4);
            break;
        }
    }
    close(r.fd);
}

/* Пауза между посетителями. Если сервер за это время что-то прислал — разбираем.
   Возвращает 1, если вход пора закрыть (CLOSE, BYE или сервер отключился) */
static int pause_and_listen(Reader *r, int ms)
{
    char line[LINE_MAX_LEN];
    struct pollfd p;
    p.fd = r->fd;
    p.events = POLLIN;
    if (!take_line(r, line, sizeof(line))) {        /* строка могла прийти раньше вместе с PARAMS */
        if (poll(&p, 1, ms) <= 0) return 0;         /* за паузу ничего не пришло */
        if (recv_line(r, line, sizeof(line)) < 0) {
            log_event(WHO_DOOR, 0, "сервер отключился — вход закрыт");
            return 1;
        }
    }
    if (strcmp(line, "CLOSE") == 0) {
        log_event(WHO_DOOR, 0, "сервер: вход закрыт — новых посетителей не впускаю");
        return 1;
    }
    if (strncmp(line, "BYE", 3) == 0) {
        log_event(WHO_DOOR, 0, "сервер: %s", line + 4);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if ((argc != 3 && argc != 4) || parse_port(argv[2]) < 0) {
        write_str(STDERR_FILENO, "Запуск: ./door IP ПОРТ [зерно]   (например ./door 127.0.0.1 5000)\n");
        return 1;
    }
    server_ip = argv[1];
    server_port = parse_port(argv[2]);
    srand(argc == 4 ? (unsigned)atoi(argv[3]) : (unsigned)(time(NULL) ^ getpid()));
    signal(SIGPIPE, SIG_IGN);

    static Reader r;
    char line[LINE_MAX_LEN];
    int arrived;
    if (say_hello(&r, server_ip, server_port, "HELLO DOOR") < 0) return 1;
    /* PARAMS всего_посетителей уже_пришло интервал_от интервал_до блюд_максимум процент_VIP */
    if (recv_line(&r, line, sizeof(line)) < 0 ||
        sscanf(line, "PARAMS %d %d %d %d %d %d", &cfg.visitors, &arrived, &cfg.arrive_min, &cfg.arrive_max,
               &cfg.max_dishes, &cfg.vip_percent) != 6) {
        write_str(STDOUT_FILENO, "Сервер прислал непонятные параметры\n");
        return 1;
    }
    if (cfg.visitors == 0)
        log_event(WHO_DOOR, 0, "открыт: посетители приходят без ограничения, раз в %d–%d мин", cfg.arrive_min, cfg.arrive_max);
    else
        log_event(WHO_DOOR, 0, "открыт: придёт %d посетителей (уже пришло %d), раз в %d–%d мин",
                  cfg.visitors, arrived, cfg.arrive_min, cfg.arrive_max);

    int n = arrived;
    int closed = 0;
    while (!closed && (cfg.visitors == 0 || n < cfg.visitors)) {
        Order o;
        make_order(&o);                     /* заказ выбираем здесь, чтобы зерно давало те же заказы */
        pid_t pid = fork();
        if (pid == 0) {                     /* процесс посетителя */
            close(r.fd);
            visitor(&o);
            _exit(0);
        }
        if (pid < 0) write_str(STDERR_FILENO, "Ошибка: не удалось создать процесс посетителя\n");
        n++;
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            /* забираем ушедших посетителей, чтобы не копились «зомби» */
        }
        if (cfg.visitors > 0 && n == cfg.visitors) break;
        closed = pause_and_listen(&r, rand_range(cfg.arrive_min, cfg.arrive_max) * cfg.delay_ms);
    }
    if (!closed) log_event(WHO_DOOR, 0, "впустил всех посетителей, жду, пока они уйдут");

    while (wait(NULL) > 0) {
        /* ждём всех посетителей */
    }
    /* дожидаемся, пока сервер скажет, что ресторан закрылся */
    while (recv_line(&r, line, sizeof(line)) == 0) {
        if (strncmp(line, "BYE", 3) == 0) {
            log_event(WHO_DOOR, 0, "сервер: %s — вход закрыт", line + 4);
            break;
        }
    }
    close(r.fd);
    return 0;
}
