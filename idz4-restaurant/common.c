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

/* Меню — как в ИДЗ 1–3 */
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

/* Готовые сценарии: название, посетители, оборудование (плита, печь, гриль, стол), блюд в заказе,
   интервал прихода, приём заказа, доставка, очередь кухни, зона готовых блюд, VIP %, приоритет, мс в минуте */
static const Config PRESETS[] = {
    { "Обычный вечер",                    12, {2, 1, 1, 2}, 3, 1, 4, 2, 1, 5, 8, 20, PRIO_FIFO, 150 },
    { "Час пик, тесная кухня",            20, {1, 1, 1, 1}, 3, 0, 2, 2, 1, 2, 4, 20, PRIO_VIP,  100 },
    { "Сломан гриль",                     10, {2, 1, 0, 2}, 2, 1, 3, 2, 1, 4, 6,  0, PRIO_FIFO, 150 },
    { "Без ограничения времени (Ctrl+C)",  0, {2, 1, 1, 2}, 3, 1, 4, 2, 1, 5, 8, 30, PRIO_VIP,  200 },
};
static const int NPRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

Config cfg;

/* write() может записать не всё сразу — тогда дописываем остаток */
void write_all(int fd, const char *s, int len)
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

/* ===================== Ввод параметров (только на сервере) ===================== */

/* Прочитать строку с клавиатуры системным вызовом read. -1 — ввод закончился */
static int read_stdin_line(char *buf, int size)
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

/* Спросить число от lo до hi; Enter — значение по умолчанию */
static int ask_int(const char *prompt, int def, int lo, int hi)
{
    char line[128], text[300];
    int value;
    char extra;
    while (1) {
        snprintf(text, sizeof(text), "  %s [%d]: ", prompt, def);
        write_str(STDOUT_FILENO, text);
        if (read_stdin_line(line, sizeof(line)) < 0) {
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
    if (read_stdin_line(line, sizeof(line)) < 0) {
        write_str(STDOUT_FILENO, "\n");
        return 0;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

static void edit_config(void)
{
    write_str(STDOUT_FILENO, "\nВведите параметры (Enter — оставить значение по умолчанию):\n");
    cfg.visitors = ask_int("Количество посетителей (0 — без ограничения, до Ctrl+C)", cfg.visitors, 0, MAX_VISITORS);
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
    cfg.cap_orders   = ask_int("Вместимость очереди заказов кухни", cfg.cap_orders, 1, MAX_VISITORS);
    cfg.cap_ready    = ask_int("Вместимость зоны готовых блюд", cfg.cap_ready, 1, 100);
    cfg.vip_percent  = ask_int("Доля VIP-посетителей, %", cfg.vip_percent, 0, 100);
    cfg.prio         = ask_int("Правило приоритета (1 — FIFO, 2 — сначала VIP)", cfg.prio, 1, 2);
    cfg.delay_ms     = ask_int("Длительность одной модельной минуты, мс", cfg.delay_ms, 1, 5000);
}

/* Выбрать сценарий и, если нужно, поменять параметры (как в ИДЗ 1–3) */
void read_config(void)
{
    char text[32];
    write_str(STDOUT_FILENO, "=== Кухня ресторана: сервер ===\n\nСценарии:\n");
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
}

void print_config(void)
{
    log_text("\n=== Сценарий: %s ===", cfg.title);
    if (cfg.visitors == 0)
        log_text("Посетители: без ограничения (вход закрывается по Ctrl+C на сервере)");
    else
        log_text("Посетителей: %d", cfg.visitors);
    log_text("Оборудование: плита — %d, печь — %d, гриль — %d, рабочий стол — %d",
             cfg.equipment[STOVE], cfg.equipment[OVEN], cfg.equipment[GRILL], cfg.equipment[TABLE]);
    log_text("Интервал появления посетителей: %d–%d мин; блюд в заказе: 1–%d; VIP: %d%%",
             cfg.arrive_min, cfg.arrive_max, cfg.max_dishes, cfg.vip_percent);
    log_text("Приём заказа: %d мин, доставка: %d мин", cfg.take_time, cfg.deliver_time);
    log_text("Вместимость: очередь заказов кухни %d, зона готовых блюд %d", cfg.cap_orders, cfg.cap_ready);
    log_text("Приоритет: %s", cfg.prio == PRIO_VIP ? "сначала VIP-посетители, затем FIFO" : "FIFO — по времени заказа");
    log_text("Одна модельная минута длится %d мс", cfg.delay_ms);
    log_text("Поваров и официантов столько, сколько клиентов подключится\n");
}

/* ===================== Вывод ===================== */

static int log_fd = -1;                 /* файл журнала (есть только у сервера) */

void open_log(const char *name)
{
    log_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0)
        write_str(STDERR_FILENO, "Не удалось открыть файл журнала, журнал писаться не будет\n");
}

void close_log(void)
{
    if (log_fd >= 0) close(log_fd);
    log_fd = -1;
}

/* Строка — в терминал и в журнал */
void log_text(const char *fmt, ...)
{
    char line[1100];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    write_all(STDOUT_FILENO, line, len);
    if (log_fd >= 0) write_all(log_fd, line, len);
}

void who_name(int who, int id, char *buf, int size)
{
    if (who == WHO_VISITOR)     snprintf(buf, size, "Посетитель %d", id + 1);
    else if (who == WHO_WAITER) snprintf(buf, size, "Официант %d", id + 1);
    else if (who == WHO_COOK)   snprintf(buf, size, "Повар %d", id + 1);
    else if (who == WHO_COORD)  snprintf(buf, size, "Координатор");
    else if (who == WHO_DOOR)   snprintf(buf, size, "Вход");
    else                        snprintf(buf, size, "Ресторан");
}

/* Событие: «[  12.5 мин] Повар 2: ...» */
void log_event(int who, int id, const char *fmt, ...)
{
    char name[64], msg[800];
    va_list ap;
    who_name(who, id, name, sizeof(name));
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_text("[%6.1f мин] %s: %s", model_time(), name, msg);
}

/* ===================== Время ===================== */

static struct timespec start;           /* момент, когда на часах ресторана было 0 минут */

static struct timespec now_time(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

/* Запустить часы так, чтобы сейчас на них было minutes модельных минут.
   Сервер вызывает с 0, клиент — с временем, которое прислал сервер */
void start_clock(double minutes)
{
    start = now_time();
    long ms = (long)(minutes * cfg.delay_ms);
    start.tv_sec -= ms / 1000;
    start.tv_nsec -= (ms % 1000) * 1000000L;
    if (start.tv_nsec < 0) {
        start.tv_sec--;
        start.tv_nsec += 1000000000L;
    }
}

double model_time(void)
{
    struct timespec now = now_time();
    double ms = (now.tv_sec - start.tv_sec) * 1000.0 + (now.tv_nsec - start.tv_nsec) / 1000000.0;
    return ms / cfg.delay_ms;
}

void sleep_ms(long ms)
{
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void sleep_minutes(int minutes)
{
    sleep_ms((long)minutes * cfg.delay_ms);
}

/* ===================== Заказы ===================== */

int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

/* Посетитель решает, что закажет */
void make_order(Order *o)
{
    memset(o, 0, sizeof(*o));
    o->vip = rand() % 100 < cfg.vip_percent;
    o->ndishes = rand_range(1, cfg.max_dishes);
    for (int i = 0; i < o->ndishes; i++)
        o->dish[i].menu = rand() % MENU_SIZE;
}

/* Заказ в сообщении: «VIP число_блюд блюдо1 блюдо2 ...», например «0 2 1 5» — не VIP, борщ и лазанья */
void order_to_text(const Order *o, char *buf, int size)
{
    int len = snprintf(buf, size, "%d %d", o->vip, o->ndishes);
    for (int i = 0; i < o->ndishes && len < size; i++)
        len += snprintf(buf + len, size - len, " %d", o->dish[i].menu);
}

/* Разобрать заказ из сообщения. -1 — в сообщении ошибка */
int order_from_text(const char *s, Order *o)
{
    int pos;
    memset(o, 0, sizeof(*o));
    if (sscanf(s, "%d %d%n", &o->vip, &o->ndishes, &pos) != 2) return -1;
    if (o->ndishes < 1 || o->ndishes > MAX_DISHES) return -1;
    for (int i = 0; i < o->ndishes; i++) {
        int m, n;
        if (sscanf(s + pos, "%d%n", &m, &n) != 1 || m < 0 || m >= MENU_SIZE) return -1;
        o->dish[i].menu = m;
        pos += n;
    }
    return 0;
}

void dishes_text(const Order *o, char *buf, int size)
{
    buf[0] = '\0';
    for (int i = 0; i < o->ndishes; i++) {
        if (i > 0) strncat(buf, ", ", size - strlen(buf) - 1);
        strncat(buf, MENU[o->dish[i].menu].name, size - strlen(buf) - 1);
    }
}

/* Правило приоритета: заказ a раньше заказа b? */
int order_before(int a, int vip_a, int b, int vip_b)
{
    if (cfg.prio == PRIO_VIP && vip_a != vip_b)
        return vip_a;
    return a < b;
}

/* Можно ли вообще выполнить заказ. NULL — можно, иначе причина */
const char *check_order(const Order *o)
{
    static char why[300];
    for (int i = 0; i < o->ndishes; i++) {
        int m = o->dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++) {
            int eq = MENU[m].ops[j].eq;
            if (cfg.equipment[eq] == 0) {
                snprintf(why, sizeof(why), "для блюда «%s» нужно оборудование «%s», а его нет", MENU[m].name, EQ_NAME[eq]);
                return why;
            }
        }
    }
    if (o->ndishes > cfg.cap_ready) {
        snprintf(why, sizeof(why), "блюд больше, чем мест в зоне готовых блюд (%d > %d)", o->ndishes, cfg.cap_ready);
        return why;
    }
    return NULL;
}
