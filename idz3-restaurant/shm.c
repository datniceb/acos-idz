#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shm.h"

Shared *sh = NULL;
sem_t *s_mutex, *s_waiter_work, *s_kitchen_free, *s_coord, *s_cook_work, *s_left;
sem_t *s_eq[EQ_TYPES];
sem_t *s_door = NULL, *s_staff = NULL, *s_gone = NULL;

static int named = 0;
static char sem_prefix[32];
static sem_t *vis_named[MAX_VISITORS];

static void fail(const char *what)
{
    char text[300];
    snprintf(text, sizeof(text), "Ошибка: %s: %s\n", what, strerror(errno));
    write_str(STDERR_FILENO, text);
    exit(1);
}

/* Создать разделяемую память. Если от прошлого запуска (убитого kill -9) что-то
   осталось, сначала удаляем старый объект */
void shm_create(const char *name)
{
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) fail("shm_open");
    if (ftruncate(fd, sizeof(Shared)) < 0) fail("ftruncate");
    sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) fail("mmap");
    close(fd);
}

int shm_attach(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return -1;
    sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sh == MAP_FAILED) fail("mmap");
    return 0;
}

void shm_detach(void)
{
    if (sh != NULL) munmap(sh, sizeof(Shared));
    sh = NULL;
}

void init_state(void)
{
    sh->cfg = cfg;
    sh->start = now_time();
    for (int t = 0; t < EQ_TYPES; t++)
        for (int u = 0; u < MAX_UNITS; u++)
            sh->equipment_busy[t][u] = -1;
}

static sem_t *make_sem(const char *name, unsigned value)
{
    char full[64];
    snprintf(full, sizeof(full), "/%s%s", sem_prefix, name);
    sem_unlink(full);
    sem_t *s = sem_open(full, O_CREAT | O_EXCL, 0600, value);
    if (s == SEM_FAILED) fail(full);
    return s;
}

static sem_t *get_sem(const char *name)
{
    char full[64];
    snprintf(full, sizeof(full), "/%s%s", sem_prefix, name);
    sem_t *s = sem_open(full, 0);
    return s == SEM_FAILED ? NULL : s;
}

void named_create(const char *prefix, int extra)
{
    char name[32];
    named = 1;
    snprintf(sem_prefix, sizeof(sem_prefix), "%s", prefix);
    s_mutex        = make_sem("mutex", 1);
    s_waiter_work  = make_sem("waiter_work", 0);
    s_kitchen_free = make_sem("kitchen_free", cfg.cap_orders);
    s_coord        = make_sem("coord", 0);
    s_cook_work    = make_sem("cook_work", 0);
    s_left         = make_sem("left", 0);
    for (int t = 0; t < EQ_TYPES; t++) {
        snprintf(name, sizeof(name), "eq%d", t);
        s_eq[t] = make_sem(name, cfg.equipment[t]);
    }
    if (extra) {
        s_door  = make_sem("door", 0);
        s_staff = make_sem("staff", 0);
        s_gone  = make_sem("gone", 0);
    }
    char full[64];
    for (int v = 0; v < MAX_VISITORS; v++) {
        snprintf(full, sizeof(full), "/%sv%d", sem_prefix, v);
        sem_unlink(full);
    }
}

int named_open(const char *prefix, int extra)
{
    char name[32];
    named = 1;
    snprintf(sem_prefix, sizeof(sem_prefix), "%s", prefix);
    s_mutex        = get_sem("mutex");
    s_waiter_work  = get_sem("waiter_work");
    s_kitchen_free = get_sem("kitchen_free");
    s_coord        = get_sem("coord");
    s_cook_work    = get_sem("cook_work");
    s_left         = get_sem("left");
    int ok = s_mutex && s_waiter_work && s_kitchen_free && s_coord && s_cook_work && s_left;
    for (int t = 0; t < EQ_TYPES; t++) {
        snprintf(name, sizeof(name), "eq%d", t);
        s_eq[t] = get_sem(name);
        if (s_eq[t] == NULL) ok = 0;
    }
    if (extra) {
        s_door  = get_sem("door");
        s_staff = get_sem("staff");
        s_gone  = get_sem("gone");
        if (!s_door || !s_staff || !s_gone) ok = 0;
    }
    return ok ? 0 : -1;
}

void named_close(void)
{
    sem_t *all[] = { s_mutex, s_waiter_work, s_kitchen_free, s_coord, s_cook_work, s_left, s_door, s_staff, s_gone };
    for (int i = 0; i < 9; i++)
        if (all[i] != NULL) sem_close(all[i]);
    for (int t = 0; t < EQ_TYPES; t++)
        if (s_eq[t] != NULL) sem_close(s_eq[t]);
    for (int v = 0; v < MAX_VISITORS; v++) {
        if (vis_named[v] != NULL) sem_close(vis_named[v]);
        vis_named[v] = NULL;
    }
}

void named_unlink(const char *prefix, int n_visitors)
{
    const char *names[] = { "mutex", "waiter_work", "kitchen_free", "coord", "cook_work", "left", "door", "staff", "gone" };
    char full[64];
    for (int i = 0; i < 9; i++) {
        snprintf(full, sizeof(full), "/%s%s", prefix, names[i]);
        sem_unlink(full);
    }
    for (int t = 0; t < EQ_TYPES; t++) {
        snprintf(full, sizeof(full), "/%seq%d", prefix, t);
        sem_unlink(full);
    }
    for (int v = 0; v < n_visitors; v++) {
        snprintf(full, sizeof(full), "/%sv%d", prefix, v);
        sem_unlink(full);
    }
}

/* Семафоры лежат в разделяемой памяти; второй аргумент sem_init = 1 —
   семафор общий для нескольких процессов */
void unnamed_init(void)
{
    named = 0;
    sem_init(&sh->u_mutex, 1, 1);
    sem_init(&sh->u_waiter_work, 1, 0);
    sem_init(&sh->u_kitchen_free, 1, cfg.cap_orders);
    sem_init(&sh->u_coord, 1, 0);
    sem_init(&sh->u_cook_work, 1, 0);
    sem_init(&sh->u_left, 1, 0);
    for (int t = 0; t < EQ_TYPES; t++) sem_init(&sh->u_eq[t], 1, cfg.equipment[t]);

    s_mutex = &sh->u_mutex;
    s_waiter_work = &sh->u_waiter_work;
    s_kitchen_free = &sh->u_kitchen_free;
    s_coord = &sh->u_coord;
    s_cook_work = &sh->u_cook_work;
    s_left = &sh->u_left;
    for (int t = 0; t < EQ_TYPES; t++) s_eq[t] = &sh->u_eq[t];
}

void unnamed_destroy(int n_visitors)
{
    sem_destroy(&sh->u_mutex);
    sem_destroy(&sh->u_waiter_work);
    sem_destroy(&sh->u_kitchen_free);
    sem_destroy(&sh->u_coord);
    sem_destroy(&sh->u_cook_work);
    sem_destroy(&sh->u_left);
    for (int t = 0; t < EQ_TYPES; t++) sem_destroy(&sh->u_eq[t]);
    for (int v = 0; v < n_visitors; v++) sem_destroy(&sh->u_visitor[v]);
}

void create_visitor_sem(int v)
{
    if (named) {
        char name[32];
        snprintf(name, sizeof(name), "v%d", v);
        vis_named[v] = make_sem(name, 0);
    } else {
        sem_init(&sh->u_visitor[v], 1, 0);
    }
}

sem_t *visitor_sem(int v)
{
    if (!named) return &sh->u_visitor[v];
    if (vis_named[v] == NULL) {
        char name[32];
        snprintf(name, sizeof(name), "v%d", v);
        vis_named[v] = get_sem(name);
        if (vis_named[v] == NULL) fail("sem_open (семафор посетителя)");
    }
    return vis_named[v];
}

void join_restaurant(void)
{
    int said = 0;
    while (1) {
        if (shm_attach("/r3_shm") == 0) {
            if (sh->kitchen_pid > 0 && kill(sh->kitchen_pid, 0) == 0 && named_open("r3_", 1) == 0)
                break;
            named_close();
            shm_detach();
        }
        if (!said) {
            write_str(STDOUT_FILENO, "Кухня ещё не открыта — ждём (сначала запустите ./shm_kitchen)...\n");
            said = 1;
        }
        sleep_ms(200);
    }
    cfg = sh->cfg;
    set_start_ptr(&sh->start);
    open_log("restaurant_shm.log", 0);
}

void staff_check(void)
{
    if (sh->cooks_here == cfg.cooks && sh->waiters_here == cfg.waiters) V(s_staff);
}

/* P и V — как на лекциях. sem_wait может прервать сигнал — тогда ждём дальше */
void P(sem_t *s)
{
    while (sem_wait(s) == -1 && errno == EINTR) {}
}

void V(sem_t *s)
{
    sem_post(s);
}
