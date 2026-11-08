#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shm.h"

static volatile sig_atomic_t ctrl_c = 0;

/* Обработчик Ctrl+C: считаем нажатия и будим кухню через sem_post
   (это одна из немногих функций, которые можно вызывать в обработчике) */
static void on_sigint(int sig)
{
    (void)sig;
    ctrl_c++;
    sem_post(s_coord);
}

static int used = 0;
static int stopped = 0;

static void check_signals(void)
{
    while (ctrl_c > used && !stopped) {
        used++;
        P(s_mutex);
        int closed = sh->intake_closed;
        V(s_mutex);
        if (!closed) {
            close_intake("получен Ctrl+C — вход закрыт, дообслуживаем тех, кто внутри");
        } else {
            stopped = 1;
            stop_all("получен Ctrl+C");
        }
    }
}

int main(void)
{
    read_config();
    open_log("restaurant_shm.log", 1);
    log_text("\n=== Программа 3: независимые процессы, именованные семафоры POSIX, разделяемая память POSIX ===");
    print_config();

    shm_create("/r3_shm");
    init_state();
    sh->kitchen_pid = getpid();
    set_start_ptr(&sh->start);
    named_create("r3_", 1);

    struct sigaction sa;
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    log_text("Кухня открыта (pid %d). Теперь запустите, например в других окнах терминала:", (int)getpid());
    log_text("  ./shm_cook — %d раз(а), ./shm_waiter — %d раз(а), ./shm_door — один раз", cfg.cooks, cfg.waiters);
    log_text("Ctrl+C в этом окне: если вход открыт — закрыть его и дообслужить тех, кто внутри;");
    log_text("если уже закрыт — остановить сразу\n");

    while (1) {
        check_signals();
        if (sem_wait(s_coord) == -1) continue;
        if (!coordinator_step()) break;
    }

    P(s_mutex);
    int n = sh->cooks_here + sh->waiters_here + sh->door_here;
    V(s_mutex);
    for (int i = 0; i < n; i++) {
        while (sem_wait(s_gone) == -1 && errno == EINTR)
            check_signals();
    }

    print_summary();
    log_text("Журнал всех процессов сохранён в файл restaurant_shm.log");
    close_log();

    int visitors = sh->n_visitors;
    named_close();
    named_unlink("r3_", visitors);
    shm_detach();
    shm_unlink("/r3_shm");
    return 0;
}
