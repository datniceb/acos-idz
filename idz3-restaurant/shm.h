#ifndef SHM_H
#define SHM_H

#include <semaphore.h>
#include <sys/types.h>

#include "common.h"

enum { V_WAIT_WAITER, V_ORDERING, V_WAIT_FOOD, V_SERVED, V_CANCELLED };
typedef struct {
    int state;
    int waiter;
    double arrive_time;
} Visitor;

/* Очереди — обычные массивы, как в ИДЗ-1 и ИДЗ-2 */
enum {
    Q_HALL,       /* посетители, ждущие официанта */
    Q_KITCHEN,    /* заказы, ждущие координатора (не больше cap_orders) */
    Q_COOKS,      /* блюда, чья очередная операция ждёт повара */
    Q_DONE,       /* блюда, у которых повар закончил операцию (сообщения координатору) */
    Q_READY,      /* собранные заказы, ждущие официанта */
    NQUEUES
};

/* Блюдо в очереди поваров хранится одним числом: заказ * MAX_DISHES + номер блюда */
#define DISH_ID(o, d)   ((o) * MAX_DISHES + (d))
#define ID_ORDER(id)    ((id) / MAX_DISHES)
#define ID_DISH(id)     ((id) % MAX_DISHES)

/* Всё, что лежит в разделяемой памяти. Меняется только под семафором s_mutex */
typedef struct {
    Config cfg;
    struct timespec start;

    Visitor visitors[MAX_VISITORS];
    Order orders[MAX_VISITORS];
    int n_visitors, n_left, n_served, n_cancelled;
    int equipment_busy[EQ_TYPES][MAX_UNITS];
    int queue[NQUEUES][QUEUE_MAX];
    int queue_len[NQUEUES];
    int zone_used;
    int zone_reserved;
    int intake_closed;
    int stop_now;
    int finish;
    int cook_ops[MAX_STAFF], cook_minutes[MAX_STAFF];
    int waiter_taken[MAX_STAFF], waiter_delivered[MAX_STAFF];
    int n_dishes_done, n_ops_done, violations;
    double total_time;
    double end_time;

    pid_t kitchen_pid;
    int cooks_here, waiters_here, door_here;

    /* только программа 2: неименованные семафоры лежат прямо в разделяемой памяти */
    sem_t u_mutex, u_waiter_work, u_kitchen_free, u_coord, u_cook_work, u_left;
    sem_t u_eq[EQ_TYPES];
    sem_t u_visitor[MAX_VISITORS];
} Shared;

extern Shared *sh;

/* Семафоры. В программах 1 и 3 они именованные (sem_open), в программе 2 — указатели на
   семафоры внутри разделяемой памяти. Остальной код разницы не видит */
extern sem_t *s_mutex;          /* «мьютекс»: защищает всё в разделяемой памяти */
extern sem_t *s_waiter_work;    /* работа для официантов: посетители в зале + собранные заказы */
extern sem_t *s_kitchen_free;   /* свободные места в очереди заказов кухни */
extern sem_t *s_coord;
extern sem_t *s_cook_work;
extern sem_t *s_left;
extern sem_t *s_eq[EQ_TYPES];   /* свободные единицы оборудования каждого типа */
extern sem_t *s_door;           /* «звонок» входу: вход закрыли или остановка */
extern sem_t *s_staff;
extern sem_t *s_gone;

void shm_create(const char *name);
int shm_attach(const char *name);
void shm_detach(void);
void init_state(void);
void named_create(const char *prefix, int extra);
int named_open(const char *prefix, int extra);
void named_close(void);
void named_unlink(const char *prefix, int n_visitors);
void unnamed_init(void);
void unnamed_destroy(int n_visitors);
void create_visitor_sem(int v);
sem_t *visitor_sem(int v);
void join_restaurant(void);
void staff_check(void);
void P(sem_t *s);
void V(sem_t *s);

int new_visitor(void);
void visitor_role(int v);
void waiter_role(int w);
void cook_role(int c);
int coordinator_step(void);
void coordinator_role(void);
void close_intake(const char *why);
void stop_all(const char *why);
void finish_all(void);
void print_summary(void);

int run_forked(int use_named);

#endif
