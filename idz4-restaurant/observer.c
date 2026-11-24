/*
 * Клиент «наблюдатель». Показывает, что происходит во всём ресторане: сервер присылает
 * ему каждое событие модели. Наблюдателей может быть сколько угодно, на разных
 * компьютерах; их можно отключать (Ctrl+C) и подключать снова, ресторан это не замечает.
 *
 * Запуск: ./observer IP ПОРТ
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

int main(int argc, char **argv)
{
    if (argc != 3 || parse_port(argv[2]) < 0) {
        write_str(STDERR_FILENO, "Запуск: ./observer IP ПОРТ   (например ./observer 127.0.0.1 5000)\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    static Reader r;
    if (say_hello(&r, argv[1], parse_port(argv[2]), "HELLO OBSERVER") < 0) return 1;
    log_text("=== Наблюдатель: ресторан на сервере %s:%s ===", argv[1], argv[2]);

    char line[LINE_MAX_LEN];
    while (recv_line(&r, line, sizeof(line)) == 0) {
        if (strncmp(line, "EV ", 3) == 0) {
            log_text("%s", line + 3);           /* событие — печатаем как есть */
        } else if (strncmp(line, "BYE", 3) == 0) {
            log_text("Сервер: %s — наблюдатель отключается", line + 4);
            close(r.fd);
            return 0;
        }
    }
    log_text("Сервер отключился — наблюдатель отключается");
    close(r.fd);
    return 0;
}
