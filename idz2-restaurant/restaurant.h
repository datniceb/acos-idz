/*
 * ИДЗ №2, вариант 32 «Кухня ресторана». Многопоточная версия.
 *
 * Из этих файлов собираются две программы:
 *   restaurant    — потоки согласуются мьютексом и семафорами (restaurant_sem.c);
 *   restaurant_cv — мьютексом и условными переменными (restaurant_cv.c).
 * Общие файлы:
 *   config.c — меню, готовые сценарии, ввод параметров с клавиатуры;
 *   util.c   — общие данные, вывод и журнал, очереди, заказы, итоги.
 */
#ifndef RESTAURANT_H
#define RESTAURANT_H

#include <pthread.h>

#define MAX_VISITORS 1000   /* максимум посетителей за один запуск */
#define MAX_STAFF    50     /* максимум официантов и поваров */
#define MAX_UNITS    20     /* максимум единиц оборудования одного типа */
#define MAX_DISHES   8      /* максимум блюд в одном заказе */
#define MAX_OPS      6      /* максимум операций в одном блюде */
#define QUEUE_MAX    1000   /* размер очередей */

/* ---------- Оборудование и меню ---------- */

enum { STOVE, OVEN, GRILL, TABLE, EQ_TYPES };   /* плита, печь, гриль, рабочий стол */

typedef struct {
    const char *name;
    int eq;         /* какое оборудование нужно */
    int time;       /* сколько минут длится */
} Operation;

/* Блюдо в меню — последовательность операций, каждая после предыдущей */
typedef struct {
    const char *name;
    int nops;
    Operation ops[MAX_OPS];
} MenuDish;

/* ---------- Параметры модели ---------- */

enum { PRIO_FIFO = 1, PRIO_VIP = 2 };

typedef struct {
    const char *title;
    int visitors;               /* 0 — без ограничения, до Ctrl+C */
    int waiters, cooks;
    int equipment[EQ_TYPES];    /* сколько плит, печей, грилей, столов */
    int max_dishes;             /* максимум блюд в заказе */
    int arrive_min, arrive_max; /* интервал появления посетителей, мин */
    int take_time;              /* сколько официант принимает заказ, мин */
    int deliver_time;           /* сколько официант несёт заказ, мин */
    int cap_orders;             /* вместимость очереди заказов кухни */
    int cap_ready;              /* вместимость зоны готовых блюд */
    int vip_percent;            /* доля VIP-посетителей, % */
    int prio;                   /* правило приоритета */
    int delay_ms;               /* сколько миллисекунд длится одна модельная минута */
} Config;

/* ---------- Заказы и посетители ---------- */

/* Блюдо в заказе */
typedef struct {
    int menu;       /* номер блюда в меню */
    int step;       /* номер текущей операции */
} Dish;

/* Заказ. Номер заказа совпадает с номером посетителя */
typedef struct {
    int vip;
    int ndishes;
    Dish dish[MAX_DISHES];
    int ready;          /* сколько блюд уже готово */
    int said_waiting;   /* уже писали, что заказ ждёт места */
} Order;

enum { V_WAIT_WAITER, V_ORDERING, V_WAIT_FOOD, V_SERVED, V_CANCELLED };
typedef struct {
    int state;
    int waiter;             /* какой официант принимает заказ */
    double arrive_time;     /* когда пришёл, мин */
} Visitor;

/* ---------- Очереди ---------- */

enum {
    Q_HALL,       /* посетители, ждущие официанта */
    Q_KITCHEN,    /* заказы, ждущие координатора (не больше cap_orders) */
    Q_COOKS,      /* блюда, чья очередная операция ждёт повара */
    Q_DONE,       /* блюда, у которых повар закончил операцию (сообщения координатору) */
    Q_READY,      /* собранные заказы, ждущие официанта */
    NQUEUES
};

/* Блюдо в очереди поваров хранится одним числом: заказ * MAX_DISHES + номер блюда.
   Например, блюдо 2 заказа 5 — это 5 * 8 + 2 = 42 */
#define DISH_ID(o, d)   ((o) * MAX_DISHES + (d))
#define ID_ORDER(id)    ((id) / MAX_DISHES)
#define ID_DISH(id)     ((id) % MAX_DISHES)

/* Кто пишет в журнал */
enum { WHO_REST, WHO_VISITOR, WHO_WAITER, WHO_COOK, WHO_COORD };

/* ---------- Общие переменные ---------- */

/* config.c */
extern const char *EQ_NAME[EQ_TYPES];
extern const char *EQ_ACC[EQ_TYPES];
extern const MenuDish MENU[];
extern const int MENU_SIZE;
extern Config cfg;

/* util.c. Всё, кроме cfg и MENU, меняется только под мьютексом mutex */
extern pthread_mutex_t mutex;
extern Visitor visitors[MAX_VISITORS];
extern Order orders[MAX_VISITORS];
extern int n_visitors;          /* сколько посетителей пришло */
extern int n_left;              /* сколько ушло */
extern int n_served, n_cancelled;
extern int equipment_busy[EQ_TYPES][MAX_UNITS];   /* номер повара или -1 */
extern int queue[NQUEUES][QUEUE_MAX];   /* элементы очередей */
extern int queue_len[NQUEUES];          /* длины очередей */
extern int zone_used;           /* блюд в зоне готовых блюд */
extern int zone_reserved;       /* мест, забронированных под заказы в работе */
extern int stop_now;            /* немедленная остановка */
extern int finish;              /* все посетители ушли, сотрудники могут расходиться */
extern int cook_ops[MAX_STAFF];     /* статистика: сколько операций сделал повар */
extern int cook_minutes[MAX_STAFF]; /* статистика: сколько минут повар готовил */
extern int n_dishes_done, n_ops_done, violations;
extern double total_time;       /* сколько минут обслуженные посетители провели в ресторане */
extern double end_time;         /* когда ресторан закрылся, мин */

/* ---------- Функции ---------- */

/* config.c */
void read_config(void);
void print_config(void);

/* util.c */
void write_str(int fd, const char *s);
void open_log(const char *name);
void close_log(void);
void start_clock(void);
double model_time(void);
void sleep_ms(long ms);
void sleep_minutes(int minutes);
void log_text(const char *fmt, ...);
void log_event(int who, int id, const char *fmt, ...);
int rand_range(int lo, int hi);
void q_push(int q, int x);
void q_remove(int q, int i);
void make_order(int v);
void dishes_text(int o, char *buf, int size);
int order_before(int a, int b);
int best_in_queue(int q);
int free_unit(int eq);
int check_order(int o);
void violation(const char *what, int id);
void print_summary(void);

#endif
