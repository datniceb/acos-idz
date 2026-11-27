/*
 * Сетевые функции: сервер слушает порт, клиенты подключаются по IP и порту.
 * Сообщения — текстовые строки, каждая заканчивается '\n' (протокол описан в отчёте).
 */
#ifndef NET_H
#define NET_H

#define LINE_MAX_LEN 1024

/* Буфер для чтения строк из сокета: TCP — это поток байтов, и одна строка может прийти
   по частям, а несколько строк — одним куском */
typedef struct {
    int fd;
    char buf[4 * LINE_MAX_LEN];
    int len;
} Reader;

int listen_on(int port);
int connect_to(const char *ip, int port);
int parse_port(const char *s);
int send_line(int fd, const char *fmt, ...);
int take_line(Reader *r, char *line, int size);
int recv_line(Reader *r, char *line, int size);
int say_hello(Reader *r, const char *ip, int port, const char *hello);

#endif
