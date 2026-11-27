/*
 * Общее для сервера и всех клиентов: меню, параметры, заказы, ввод параметров, вывод, время.
 */
#ifndef COMMON_H
#define COMMON_H

#include <time.h>

#define MAX_VISITORS 200        /* у каждого посетителя своё соединение, а дескрипторов у процесса не бесконечно */
#define MAX_STAFF    20         /* поваров и официантов — каждого вида */
#define MAX_UNITS    20
#define MAX_DISHES   8
#define MAX_OPS      6

enum { STOVE, OVEN, GRILL, TABLE, EQ_TYPES };   /* плита, печь, гриль, рабочий стол */

typedef struct {
    const char *name;
    int eq;                     /* какое оборудование нужно */
    int time;                   /* сколько минут длится */
} Operation;

typedef struct {
    const char *name;
    int nops;
    Operation ops[MAX_OPS];     /* операции выполняются по порядку */
} MenuDish;

enum { PRIO_FIFO = 1, PRIO_VIP = 2 };

/* Параметры. Их вводят на сервере, клиенты получают нужные им при подключении */
typedef struct {
    char title[64];
    int visitors;               /* 0 — без ограничения, до Ctrl+C */
    int equipment[EQ_TYPES];
    int max_dishes;
    int arrive_min, arrive_max;
    int take_time;
    int deliver_time;
    int cap_orders;
    int cap_ready;
    int vip_percent;
    int prio;
    int delay_ms;               /* сколько миллисекунд длится одна модельная минута */
} Config;

typedef struct {
    int menu;                   /* номер блюда в меню */
    int step;                   /* сколько операций уже сделано */
} Dish;

typedef struct {
    int vip;
    int ndishes;
    Dish dish[MAX_DISHES];
    int ready;                  /* сколько блюд готово */
    int said_waiting;           /* уже писали, что заказ ждёт места в зоне */
} Order;

enum { WHO_REST, WHO_VISITOR, WHO_WAITER, WHO_COOK, WHO_COORD, WHO_DOOR };

extern const char *EQ_NAME[EQ_TYPES];
extern const char *EQ_ACC[EQ_TYPES];
extern const MenuDish MENU[];
extern const int MENU_SIZE;
extern Config cfg;

void read_config(void);
void print_config(void);
void write_str(int fd, const char *s);
void write_all(int fd, const char *s, int len);

void open_log(const char *name);
void close_log(void);
void log_text(const char *fmt, ...);
void who_name(int who, int id, char *buf, int size);
void log_event(int who, int id, const char *fmt, ...);

void start_clock(double minutes);
double model_time(void);
void sleep_ms(long ms);
void sleep_minutes(int minutes);

int rand_range(int lo, int hi);
void make_order(Order *o);
void order_to_text(const Order *o, char *buf, int size);
int order_from_text(const char *s, Order *o);
void dishes_text(const Order *o, char *buf, int size);
int order_before(int a, int vip_a, int b, int vip_b);
const char *check_order(const Order *o);

#endif
