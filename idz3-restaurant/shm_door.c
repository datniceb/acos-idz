#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shm.h"

int main(void)
{
    char text[200];

    join_restaurant();
    signal(SIGINT, SIG_IGN);
    srand(time(NULL) ^ getpid());

    P(s_mutex);
    if (sh->stop_now || sh->finish || sh->door_here) {
        V(s_mutex);
        write_str(STDOUT_FILENO, "Вход уже работает (или ресторан закрыт)\n");
        named_close();
        shm_detach();
        return 0;
    }
    sh->door_here = 1;
    log_text("Вход ждёт, пока соберутся все сотрудники (pid входа %d)", (int)getpid());
    V(s_mutex);

    P(s_staff);                 /* последний пришедший сотрудник открывает вход */
    P(s_mutex);
    if (!sh->stop_now) {
        sh->start = now_time();
        log_event(WHO_REST, 0, "все сотрудники на месте — вход открыт");
    }
    V(s_mutex);

    while (1) {
        P(s_mutex);
        int closed = sh->intake_closed || sh->stop_now;
        V(s_mutex);
        if (closed) break;

        int v = new_visitor();
        pid_t pid = fork();
        if (pid == 0) {
            visitor_role(v);
            named_close();
            shm_detach();
            _exit(0);
        }
        if (pid < 0) write_str(STDERR_FILENO, "Ошибка: не удалось создать процесс посетителя\n");

        if (cfg.visitors > 0 && v + 1 == cfg.visitors) {
            snprintf(text, sizeof(text), "вход закрыт: пришли все %d посетителей", cfg.visitors);
            close_intake(text);
            break;
        }
        if (v + 1 == MAX_VISITORS) {
            snprintf(text, sizeof(text), "вход закрыт: в модели не может быть больше %d посетителей", MAX_VISITORS);
            close_intake(text);
            break;
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
        struct timespec until = deadline_ms((long)rand_range(cfg.arrive_min, cfg.arrive_max) * cfg.delay_ms);
        while (sem_timedwait(s_door, &until) == -1 && errno == EINTR) {}
    }

    for (int left = 0; left < sh->n_visitors; left++) P(s_left);
    finish_all();
    while (wait(NULL) > 0) {}

    V(s_gone);
    named_close();
    shm_detach();
    close_log();
    return 0;
}
