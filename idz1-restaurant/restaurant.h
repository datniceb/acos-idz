/*
 * ИДЗ №1, вариант 32 «Кухня ресторана».
 * Общий заголовок: константы, типы данных, глобальные переменные, функции.
 */
#ifndef RESTAURANT_H
#define RESTAURANT_H

#include <signal.h>

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
    int delay_ms;               /* пауза на одну модельную минуту, мс */
} Config;

/* ---------- Участники модели ---------- */

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

enum { V_WAIT_WAITER, V_ORDERING, V_WAIT_FOOD, V_SERVED, V_LEFT };
typedef struct {
    int state;
    long arrive_time;
} Visitor;

enum { W_FREE, W_TAKING, W_AT_KITCHEN, W_DELIVERING };
typedef struct {
    int state;
    int visitor;    /* у кого принимает заказ */
    int slip;       /* заказ, который несёт на кухню (-1 — нет) */
    int carrying;   /* готовый заказ, который несёт посетителю (-1 — нет) */
} Waiter;

typedef struct {
    int busy;       /* 1 — готовит */
    int order, dish;
    int eq, unit;   /* какое оборудование занял */
} Cook;

/* ---------- Очереди ---------- */

enum {
    Q_HALL,       /* посетители, ждущие официанта */
    Q_KITCHEN,    /* заказы, ждущие координатора */
    Q_COOKS,      /* блюда, чья очередная операция ждёт повара */
    Q_READY,      /* собранные заказы, ждущие официанта */
    NQUEUES
};

/* ---------- События ---------- */

enum { EV_ARRIVAL, EV_ORDER_TAKEN, EV_COOK_DONE, EV_DELIVERED };
typedef struct {
    long time;      /* модельное время, мин */
    long num;       /* порядковый номер — чтобы события одной минуты шли по порядку */
    int type;
    int who;        /* номер официанта или повара */
} Event;

/* Кто пишет в журнал */
enum { WHO_REST, WHO_VISITOR, WHO_WAITER, WHO_COOK, WHO_COORD };

/* ---------- Глобальные переменные ---------- */

extern const char *EQ_NAME[EQ_TYPES];
extern const char *EQ_ACC[EQ_TYPES];
extern const MenuDish MENU[];
extern const int MENU_SIZE;
extern const Config PRESETS[];
extern const int NPRESETS;
extern Config cfg;

extern long now;                            /* текущее модельное время, мин */
extern Visitor visitors[MAX_VISITORS];
extern Order orders[MAX_VISITORS];
extern int n_visitors;
extern Waiter waiters[MAX_STAFF];
extern Cook cooks[MAX_STAFF];
extern int equipment_busy[EQ_TYPES][MAX_UNITS];   /* номер повара или -1 */

extern int queue[NQUEUES][QUEUE_MAX];   /* элементы очередей */
extern int queue_len[NQUEUES];          /* длины очередей */

extern int zone_used;         /* блюд в зоне готовых блюд */
extern int zone_reserved;     /* мест, забронированных под принятые заказы */
extern int intake_closed, n_served, n_cancelled;
extern long total_wait;
extern volatile sig_atomic_t stop_flag;

/* ---------- Функции ---------- */

/* config.c */
int ask_int(const char *prompt, int def, int lo, int hi);
int ask_yes(const char *prompt);
void edit_config(void);
void print_config(void);

/* events.c */
void add_event(long time, int type, int who);
int no_events(void);
Event next_event(void);

/* hall.c — посетители и официанты */
void visitor_arrives(void);
int waiters_work(void);
void order_taken(int w);
void order_delivered(int w);

/* kitchen.c — координатор кухни и повара */
int order_before(int a, int b);
int check_order(int o);
int coordinator_work(void);
int cooks_work(void);
void cook_done(int c);

/* util.c */
void log_who(int who, int id);
void print_dishes(int o);
int rand_range(int lo, int hi);
void sleep_ms(long ms);
void q_push(int q, int x);
void q_remove(int q, int i);

#endif
