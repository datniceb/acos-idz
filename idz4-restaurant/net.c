#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

/* Сервер: создать сокет, который принимает подключения на порт port с любого адреса */
int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));   /* порт можно сразу занять снова */

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Клиент: подключиться к серверу. Если сервер ещё не запущен, пробуем снова
   пять раз в секунду (до 150 раз — полминуты). -1 — не получилось */
int connect_to(const char *ip, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        write_str(STDERR_FILENO, "Ошибка: неверный IP-адрес (пример: 127.0.0.1)\n");
        return -1;
    }
    for (int i = 0; i < 150; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        if (errno != ECONNREFUSED) break;
        if (i == 0) write_str(STDOUT_FILENO, "Сервер ещё не запущен — жду...\n");
        sleep_ms(200);
    }
    write_str(STDERR_FILENO, "Ошибка: не удалось подключиться к серверу\n");
    return -1;
}

int parse_port(const char *s)
{
    int port = atoi(s);
    return (port > 0 && port < 65536) ? port : -1;
}

/* Отправить одну строку (сама добавляет '\n'). -1 — соединение разорвано */
int send_line(int fd, const char *fmt, ...)
{
    char line[LINE_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    int done = 0;
    while (done < len) {
        int n = write(fd, line + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += n;
    }
    return 0;
}

/* Вынуть из буфера одну целую строку (без '\n'). 1 — вынули, 0 — целой строки ещё нет */
int take_line(Reader *r, char *line, int size)
{
    for (int i = 0; i < r->len; i++) {
        if (r->buf[i] == '\n') {
            int n = i < size - 1 ? i : size - 1;
            memcpy(line, r->buf, n);
            line[n] = '\0';
            memmove(r->buf, r->buf + i + 1, r->len - i - 1);
            r->len -= i + 1;
            return 1;
        }
    }
    return 0;
}

/* Клиент: дождаться и прочитать строку от сервера. -1 — сервер закрыл соединение */
int recv_line(Reader *r, char *line, int size)
{
    while (!take_line(r, line, size)) {
        if (r->len == (int)sizeof(r->buf)) return -1;      /* слишком длинная строка — так не бывает */
        int n = read(r->fd, r->buf + r->len, sizeof(r->buf) - r->len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        r->len += n;
    }
    return 0;
}

/* Клиент: подключиться, представиться серверу (строка hello) и получить в ответ
   «WELCOME номер время мс_в_минуте». Часы клиента ставятся по часам сервера.
   Возвращает номер, который выдал сервер, или -1, если сервер отказал или недоступен */
int say_hello(Reader *r, const char *ip, int port, const char *hello)
{
    char line[LINE_MAX_LEN];
    int id;
    double t;
    r->fd = connect_to(ip, port);
    r->len = 0;
    if (r->fd < 0) return -1;
    if (send_line(r->fd, "%s", hello) < 0 || recv_line(r, line, sizeof(line)) < 0) {
        write_str(STDOUT_FILENO, "Сервер закрыл соединение\n");
        return -1;
    }
    if (sscanf(line, "WELCOME %d %lf %d", &id, &t, &cfg.delay_ms) != 3) {
        if (strncmp(line, "BYE ", 4) == 0) {
            write_str(STDOUT_FILENO, "Сервер отказал: ");
            write_str(STDOUT_FILENO, line + 4);
            write_str(STDOUT_FILENO, "\n");
        }
        close(r->fd);
        return -1;
    }
    start_clock(t);
    return id;
}
