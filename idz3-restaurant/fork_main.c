#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shm.h"

enum { KIND_COORD, KIND_COOK, KIND_WAITER, KIND_VISITOR };

static volatile sig_atomic_t ctrl_c = 0;

static void on_sigint(int sig)
{
    (void)sig;
    ctrl_c++;
}

static void start_child(int kind, int id, int use_named)
{
    pid_t pid = fork();
    if (pid < 0) {
        write_str(STDERR_FILENO, "Ошибка: не удалось создать процесс (fork)\n");
        return;
    }
    if (pid > 0) return;

    /* дочерний процесс. Ctrl+C из терминала приходит всей группе процессов,
       но обрабатывает его только родитель — дочерние сигнал игнорируют */
    signal(SIGINT, SIG_IGN);
    if (kind == KIND_COORD) coordinator_role();
    else if (kind == KIND_COOK) cook_role(id);
    else if (kind == KIND_WAITER) waiter_role(id);
    else visitor_role(id);
    if (use_named) named_close();
    shm_detach();
    _exit(0);
}

int run_forked(int use_named)
{
    const char *shm_name = use_named ? "/r1_shm" : "/r2_shm";
    const char *log_name = use_named ? "restaurant_named.log" : "restaurant_unnamed.log";
    char text[200];

    srand(time(NULL));
    read_config();
    open_log(log_name, 1);
    if (use_named)
        log_text("\n=== Программа 1: процессы (fork), именованные семафоры POSIX, разделяемая память POSIX ===");
    else
        log_text("\n=== Программа 2: процессы (fork), неименованные семафоры в разделяемой памяти POSIX ===");
    print_config();

    /* разделяемая память и семафоры — до fork, тогда дочерние процессы их унаследуют */
    shm_create(shm_name);
    init_state();
    set_start_ptr(&sh->start);
    if (use_named)
        named_create("r1_", 0);
    else
        unnamed_init();

    /* Ctrl+C обрабатывает родитель. Без SA_RESTART: сигнал прерывает
       nanosleep и sem_wait, и родитель сразу видит, что пришёл сигнал */
    struct sigaction sa;
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    start_child(KIND_COORD, 0, use_named);
    for (int c = 0; c < cfg.cooks; c++) start_child(KIND_COOK, c, use_named);
    for (int w = 0; w < cfg.waiters; w++) start_child(KIND_WAITER, w, use_named);

    if (cfg.visitors == 0)
        log_text("Режим без ограничения времени: посетители приходят, пока не нажать Ctrl+C");
    log_text("Ctrl+C: если вход открыт — закрыть его и дообслужить тех, кто внутри; если уже закрыт — остановить сразу");
    log_text("Процесс-родитель (вход): pid %d\n", (int)getpid());
    P(s_mutex);
    log_event(WHO_REST, 0, "ресторан открыт: координатор, поваров — %d, официантов — %d", cfg.cooks, cfg.waiters);
    V(s_mutex);

    int used = 0;
    while (1) {
        if (ctrl_c > 0) {
            used = 1;
            close_intake("получен Ctrl+C — вход закрыт, дообслуживаем тех, кто внутри");
            break;
        }
        int v = new_visitor();
        start_child(KIND_VISITOR, v, use_named);
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
        sleep_minutes(rand_range(cfg.arrive_min, cfg.arrive_max));
    }

    int left = 0, stopped = 0;
    while (left < sh->n_visitors) {
        if (!stopped && ctrl_c > used) {
            stopped = 1;
            stop_all("получен Ctrl+C");
        }
        if (sem_wait(s_left) == 0) left++;
    }

    finish_all();
    while (wait(NULL) > 0) {}

    print_summary();
    log_text("Журнал сохранён в файл %s", log_name);
    close_log();

    int n = sh->n_visitors;
    if (use_named) {
        named_close();
        named_unlink("r1_", n);
    } else {
        unnamed_destroy(n);
    }
    shm_detach();
    shm_unlink(shm_name);
    return 0;
}
