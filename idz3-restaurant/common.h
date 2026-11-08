#ifndef COMMON_H
#define COMMON_H

#include <time.h>

#define MAX_VISITORS 1000
#define MAX_STAFF    50
#define MAX_UNITS    20
#define MAX_DISHES   8
#define MAX_OPS      6
#define QUEUE_MAX    1000

enum { STOVE, OVEN, GRILL, TABLE, EQ_TYPES };   /* плита, печь, гриль, рабочий стол */

typedef struct {
    const char *name;
    int eq;
    int time;
} Operation;

typedef struct {
    const char *name;
    int nops;
    Operation ops[MAX_OPS];
} MenuDish;

enum { PRIO_FIFO = 1, PRIO_VIP = 2 };

typedef struct {
    char title[64];
    int visitors;               /* 0 — без ограничения, до Ctrl+C */
    int waiters, cooks;
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
    int menu;
    int step;
} Dish;

typedef struct {
    int vip;
    int ndishes;
    Dish dish[MAX_DISHES];
    int ready;
    int said_waiting;
} Order;

enum { WHO_REST, WHO_VISITOR, WHO_WAITER, WHO_COOK, WHO_COORD };

extern const char *EQ_NAME[EQ_TYPES];
extern const char *EQ_ACC[EQ_TYPES];
extern const MenuDish MENU[];
extern const int MENU_SIZE;
extern Config cfg;

void read_config(void);
void print_config(void);
void write_str(int fd, const char *s);

void open_log(const char *name, int truncate);
void close_log(void);
void log_text(const char *fmt, ...);
void log_event(int who, int id, const char *fmt, ...);

void set_start(struct timespec t);
void set_start_ptr(struct timespec *p);
struct timespec now_time(void);
double model_time(void);
void sleep_ms(long ms);
void sleep_minutes(int minutes);
struct timespec deadline_ms(long ms);

int rand_range(int lo, int hi);
void make_order(Order *o);
void dishes_text(const Order *o, char *buf, int size);
int order_before(int a, int vip_a, int b, int vip_b);
int check_order(const Order *o, int num);

#endif
