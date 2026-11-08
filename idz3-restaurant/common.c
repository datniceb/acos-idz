#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

const char *EQ_NAME[EQ_TYPES] = { "плита", "печь", "гриль", "рабочий стол" };
const char *EQ_ACC[EQ_TYPES]  = { "плиту", "печь", "гриль", "рабочий стол" };

/* Меню — как в ИДЗ-1 и ИДЗ-2 */
const MenuDish MENU[] = {
    { "Стейк с картофелем", 4, {
        { "разделка мяса",     TABLE, 3 },
        { "жарка стейка",      GRILL, 6 },
        { "варка картофеля",   STOVE, 5 },
        { "подача на тарелку", TABLE, 1 } } },
    { "Борщ", 2, {
        { "нарезка овощей",    TABLE, 3 },
        { "варка",             STOVE, 8 } } },
    { "Пицца", 2, {
        { "раскатка теста",    TABLE, 3 },
        { "выпечка",           OVEN,  7 } } },
    { "Салат Цезарь", 3, {
        { "обжарка курицы",    GRILL, 4 },
        { "нарезка листьев",   TABLE, 2 },
        { "сборка салата",     TABLE, 1 } } },
    { "Омлет", 2, {
        { "взбивание яиц",     TABLE, 1 },
        { "жарка омлета",      STOVE, 3 } } },
    { "Лазанья", 3, {
        { "варка листов",      STOVE, 4 },
        { "сборка лазаньи",    TABLE, 3 },
        { "запекание",         OVEN,  9 } } },
};
const int MENU_SIZE = sizeof(MENU) / sizeof(MENU[0]);

static const Config PRESETS[] = {
    { "Обычный вечер",                    12, 3, 4, {2, 1, 1, 2}, 3, 1, 4, 2, 1, 5, 8, 20, PRIO_FIFO, 150 },
    { "Час пик, тесная кухня",            20, 2, 3, {1, 1, 1, 1}, 3, 0, 2, 2, 1, 2, 4, 20, PRIO_VIP,  100 },
    { "Сломан гриль",                     10, 2, 3, {2, 1, 0, 2}, 2, 1, 3, 2, 1, 4, 6,  0, PRIO_FIFO, 150 },
    { "Без ограничения времени (Ctrl+C)",  0, 3, 4, {2, 1, 1, 2}, 3, 1, 4, 2, 1, 5, 8, 30, PRIO_VIP,  200 },
};
static const int NPRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

Config cfg;

/* write() может записать не всё сразу — тогда дописываем остаток.
   Если запись прервал сигнал (EINTR), пробуем ещё раз */
static void write_all(int fd, const char *s, int len)
{
    while (len > 0) {
        int n = write(fd, s, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        s += n;
        len -= n;
    }
}

void write_str(int fd, const char *s)
{
    write_all(fd, s, strlen(s));
}

static int read_line(char *buf, int size)
{
    int len = 0;
    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == '\n') {
            buf[len] = '\0';
            return len;
        }
        if (len < size - 1) buf[len++] = ch;
    }
    buf[len] = '\0';
    return len > 0 ? len : -1;
}

static int ask_int(const char *prompt, int def, int lo, int hi)
{
    char line[128], text[300];
    int value;
    char extra;
    while (1) {
        snprintf(text, sizeof(text), "  %s [%d]: ", prompt, def);
        write_str(STDOUT_FILENO, text);
        if (read_line(line, sizeof(line)) < 0) {
            write_str(STDOUT_FILENO, "\n");
            return def;
        }
        if (line[0] == '\0') return def;
        if (sscanf(line, "%d %c", &value, &extra) == 1 && value >= lo && value <= hi) return value;
        snprintf(text, sizeof(text), "    Ошибка: нужно целое число от %d до %d\n", lo, hi);
        write_str(STDOUT_FILENO, text);
    }
}

static int ask_yes(const char *prompt)
{
    char line[64];
    write_str(STDOUT_FILENO, prompt);
    write_str(STDOUT_FILENO, " [y/N]: ");
    if (read_line(line, sizeof(line)) < 0) {
        write_str(STDOUT_FILENO, "\n");
        return 0;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

static void edit_config(void)
{
    write_str(STDOUT_FILENO, "\nВведите параметры (Enter — оставить значение по умолчанию):\n");
    cfg.visitors = ask_int("Количество посетителей (0 — без ограничения, до Ctrl+C)", cfg.visitors, 0, MAX_VISITORS);
    cfg.waiters  = ask_int("Количество официантов", cfg.waiters, 1, MAX_STAFF);
    cfg.cooks    = ask_int("Количество поваров", cfg.cooks, 0, MAX_STAFF);
    cfg.equipment[STOVE] = ask_int("Количество плит", cfg.equipment[STOVE], 0, MAX_UNITS);
    cfg.equipment[OVEN]  = ask_int("Количество печей", cfg.equipment[OVEN], 0, MAX_UNITS);
    cfg.equipment[GRILL] = ask_int("Количество грилей", cfg.equipment[GRILL], 0, MAX_UNITS);
    cfg.equipment[TABLE] = ask_int("Количество рабочих столов", cfg.equipment[TABLE], 0, MAX_UNITS);
    cfg.max_dishes   = ask_int("Максимум блюд в заказе", cfg.max_dishes, 1, MAX_DISHES);
    cfg.arrive_min   = ask_int("Минимальный интервал появления посетителей, мин", cfg.arrive_min, 0, 120);
    if (cfg.arrive_max < cfg.arrive_min) cfg.arrive_max = cfg.arrive_min;
    cfg.arrive_max   = ask_int("Максимальный интервал появления посетителей, мин", cfg.arrive_max, cfg.arrive_min, 240);
    cfg.take_time    = ask_int("Время приёма заказа официантом, мин", cfg.take_time, 1, 60);
    cfg.deliver_time = ask_int("Время доставки заказа посетителю, мин", cfg.deliver_time, 1, 60);
    cfg.cap_orders   = ask_int("Вместимость очереди заказов кухни", cfg.cap_orders, 1, QUEUE_MAX);
    cfg.cap_ready    = ask_int("Вместимость зоны готовых блюд", cfg.cap_ready, 1, QUEUE_MAX);
    cfg.vip_percent  = ask_int("Доля VIP-посетителей, %", cfg.vip_percent, 0, 100);
    cfg.prio         = ask_int("Правило приоритета (1 — FIFO, 2 — сначала VIP)", cfg.prio, 1, 2);
    cfg.delay_ms     = ask_int("Длительность одной модельной минуты, мс", cfg.delay_ms, 1, 5000);
}

/* Выбрать сценарий и, если нужно, поменять параметры (как в ИДЗ-1 и ИДЗ-2) */
void read_config(void)
{
    char text[100];
    write_str(STDOUT_FILENO, "=== Кухня ресторана: взаимодействующие процессы ===\n\nСценарии:\n");
    for (int i = 0; i < NPRESETS; i++) {
        snprintf(text, sizeof(text), "  %d — ", i + 1);
        write_str(STDOUT_FILENO, text);
        write_str(STDOUT_FILENO, PRESETS[i].title);
        write_str(STDOUT_FILENO, "\n");
    }
    int p = ask_int("Выберите сценарий", 1, 1, NPRESETS);
    cfg = PRESETS[p - 1];
    if (ask_yes("Изменить параметры сценария?")) {
        edit_config();
        strcpy(cfg.title, "свои параметры");
    }
    if (cfg.visitors == 0 && cfg.delay_ms < 10) cfg.delay_ms = 10;
}

void print_config(void)
{
    log_text("\n=== Сценарий: %s ===", cfg.title);
    if (cfg.visitors == 0)
        log_text("Посетители: без ограничения (вход закрывается по Ctrl+C)");
    else
        log_text("Посетителей: %d", cfg.visitors);
    log_text("Официантов: %d, поваров: %d", cfg.waiters, cfg.cooks);
    log_text("Оборудование: плита — %d, печь — %d, гриль — %d, рабочий стол — %d",
             cfg.equipment[STOVE], cfg.equipment[OVEN], cfg.equipment[GRILL], cfg.equipment[TABLE]);
    log_text("Интервал появления посетителей: %d–%d мин; блюд в заказе: 1–%d; VIP: %d%%",
             cfg.arrive_min, cfg.arrive_max, cfg.max_dishes, cfg.vip_percent);
    log_text("Приём заказа: %d мин, доставка: %d мин", cfg.take_time, cfg.deliver_time);
    log_text("Вместимость: очередь заказов кухни %d, зона готовых блюд %d", cfg.cap_orders, cfg.cap_ready);
    log_text("Приоритет: %s", cfg.prio == PRIO_VIP ? "сначала VIP-посетители, затем FIFO" : "FIFO — по времени заказа");
    log_text("Одна модельная минута длится %d мс", cfg.delay_ms);

    log_text("\nМеню (операции выполняются по порядку):");
    for (int m = 0; m < MENU_SIZE; m++) {
        log_text("  %s", MENU[m].name);
        for (int j = 0; j < MENU[m].nops; j++)
            log_text("    %d) %s — %s, %d мин", j + 1, MENU[m].ops[j].name,
                     EQ_NAME[MENU[m].ops[j].eq], MENU[m].ops[j].time);
    }
    log_text("");
}

static int log_fd = -1;
static struct timespec my_start;
static struct timespec *start = &my_start;

/* Журнал общий для всех процессов. O_APPEND — каждая строка дописывается в конец файла,
   даже если пишут несколько процессов сразу */
void open_log(const char *name, int truncate)
{
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (truncate) flags |= O_TRUNC;
    log_fd = open(name, flags, 0644);
    if (log_fd < 0)
        write_str(STDERR_FILENO, "Не удалось открыть файл журнала, журнал писаться не будет\n");
}

void close_log(void)
{
    if (log_fd >= 0) close(log_fd);
    log_fd = -1;
}

static void out(const char *line, int len)
{
    write_all(STDOUT_FILENO, line, len);
    if (log_fd >= 0) write_all(log_fd, line, len);
}

void log_text(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    out(line, len);
}

void log_event(int who, int id, const char *fmt, ...)
{
    char name[64], msg[800], line[1024];
    va_list ap;

    if (who == WHO_VISITOR)     snprintf(name, sizeof(name), "Посетитель %d", id + 1);
    else if (who == WHO_WAITER) snprintf(name, sizeof(name), "Официант %d", id + 1);
    else if (who == WHO_COOK)   snprintf(name, sizeof(name), "Повар %d", id + 1);
    else if (who == WHO_COORD)  snprintf(name, sizeof(name), "Координатор");
    else                        snprintf(name, sizeof(name), "Ресторан");

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    int len = snprintf(line, sizeof(line), "[%6.1f мин] %s: %s\n", model_time(), name, msg);
    if (len > (int)sizeof(line) - 1) len = sizeof(line) - 1;
    out(line, len);
}

void set_start(struct timespec t)
{
    my_start = t;
}

void set_start_ptr(struct timespec *p)
{
    start = p;
}

struct timespec now_time(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

double model_time(void)
{
    struct timespec now = now_time();
    double ms = (now.tv_sec - start->tv_sec) * 1000.0 + (now.tv_nsec - start->tv_nsec) / 1000000.0;
    return ms / cfg.delay_ms;
}

void sleep_ms(long ms)
{
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void sleep_minutes(int minutes)
{
    sleep_ms((long)minutes * cfg.delay_ms);
}

struct timespec deadline_ms(long ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

void make_order(Order *o)
{
    o->vip = rand() % 100 < cfg.vip_percent;
    o->ndishes = rand_range(1, cfg.max_dishes);
    o->ready = 0;
    o->said_waiting = 0;
    for (int i = 0; i < o->ndishes; i++) {
        o->dish[i].menu = rand() % MENU_SIZE;
        o->dish[i].step = 0;
    }
}

void dishes_text(const Order *o, char *buf, int size)
{
    buf[0] = '\0';
    for (int i = 0; i < o->ndishes; i++) {
        if (i > 0) strncat(buf, ", ", size - strlen(buf) - 1);
        strncat(buf, MENU[o->dish[i].menu].name, size - strlen(buf) - 1);
    }
}

int order_before(int a, int vip_a, int b, int vip_b)
{
    if (cfg.prio == PRIO_VIP && vip_a != vip_b)
        return vip_a;
    return a < b;
}

int check_order(const Order *o, int num)
{
    if (cfg.cooks == 0) {
        log_event(WHO_COORD, 0, "заказ #%d невыполним: на кухне нет поваров. Заказ отменён", num + 1);
        return 0;
    }
    for (int i = 0; i < o->ndishes; i++) {
        int m = o->dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++) {
            int eq = MENU[m].ops[j].eq;
            if (cfg.equipment[eq] == 0) {
                log_event(WHO_COORD, 0, "заказ #%d невыполним: для блюда «%s» нужно оборудование «%s», а его нет. Заказ отменён",
                          num + 1, MENU[m].name, EQ_NAME[eq]);
                return 0;
            }
        }
    }
    if (o->ndishes > cfg.cap_ready) {
        log_event(WHO_COORD, 0, "заказ #%d невыполним: блюд больше, чем мест в зоне готовых блюд (%d > %d). Заказ отменён",
                  num + 1, o->ndishes, cfg.cap_ready);
        return 0;
    }
    return 1;
}
