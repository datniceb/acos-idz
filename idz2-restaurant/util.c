/* Общие данные модели, вывод и журнал, очереди, заказы, итоги */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "restaurant.h"

/* Один мьютекс на всё состояние ресторана: очереди, заказы, оборудование, счётчики.
   Внутри него никто не спит — только быстро меняет данные */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

Visitor visitors[MAX_VISITORS];
Order orders[MAX_VISITORS];
int n_visitors, n_left, n_served, n_cancelled;
int equipment_busy[EQ_TYPES][MAX_UNITS];
int queue[NQUEUES][QUEUE_MAX];
int queue_len[NQUEUES];
int zone_used, zone_reserved;
int stop_now, finish;
int cook_ops[MAX_STAFF], cook_minutes[MAX_STAFF];
int n_dishes_done, n_ops_done, violations;
double total_time, end_time;

/* ===================== Вывод и журнал ===================== */

/* Строки печатаются под отдельным мьютексом, чтобы строки разных потоков не перемешивались */
static pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;
static int log_fd = -1;             /* файл журнала */
static struct timespec start;       /* когда открылся ресторан */

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

/* write() может записать не всё сразу — тогда дописываем остаток */
static void write_all(int fd, const char *s, int len)
{
    while (len > 0) {
        int n = write(fd, s, len);
        if (n <= 0) return;
        s += n;
        len -= n;
    }
}

void write_str(int fd, const char *s)
{
    write_all(fd, s, strlen(s));
}

/* Записать строку в терминал (дескриптор 1) и в журнал */
static void out(const char *line, int len)
{
    pthread_mutex_lock(&out_mutex);
    write_all(STDOUT_FILENO, line, len);
    if (log_fd >= 0) write_all(log_fd, line, len);
    pthread_mutex_unlock(&out_mutex);
}

/* Обычная строка (параметры, итоги) */
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

/* Событие модели: «[  12.5 мин] Повар 2: ...» */
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

    /* время берём под мьютексом вывода, чтобы в журнале оно не шло назад */
    pthread_mutex_lock(&out_mutex);
    int len = snprintf(line, sizeof(line), "[%6.1f мин] %s: %s\n", model_time(), name, msg);
    if (len > (int)sizeof(line) - 1) len = sizeof(line) - 1;
    write_all(STDOUT_FILENO, line, len);
    if (log_fd >= 0) write_all(log_fd, line, len);
    pthread_mutex_unlock(&out_mutex);
}

/* ===================== Время ===================== */

void start_clock(void)
{
    clock_gettime(CLOCK_MONOTONIC, &start);
}

/* Модельное время в минутах: сколько прошло с открытия, делённое на длину минуты */
double model_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double ms = (now.tv_sec - start.tv_sec) * 1000.0 + (now.tv_nsec - start.tv_nsec) / 1000000.0;
    return ms / cfg.delay_ms;
}

/* Пауза. Если её прервёт сигнал (Ctrl+C в главном потоке), nanosleep вернётся раньше */
void sleep_ms(long ms)
{
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Пауза на minutes модельных минут: так участник «выполняет» действие */
void sleep_minutes(int minutes)
{
    sleep_ms((long)minutes * cfg.delay_ms);
}

/* ===================== Очереди и заказы ===================== */

/* Случайное целое от lo до hi включительно. rand() вызывает только главный поток */
int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

/* Добавить x в конец очереди q */
void q_push(int q, int x)
{
    if (queue_len[q] < QUEUE_MAX) {
        queue[q][queue_len[q]] = x;
        queue_len[q]++;
    }
}

/* Удалить i-й элемент очереди q, остальные сдвигаются вперёд */
void q_remove(int q, int i)
{
    for (int k = i; k < queue_len[q] - 1; k++)
        queue[q][k] = queue[q][k + 1];
    queue_len[q]--;
}

/* Посетитель v решает, что закажет: от 1 до max_dishes случайных блюд меню */
void make_order(int v)
{
    visitors[v].state = V_WAIT_WAITER;
    visitors[v].waiter = -1;
    orders[v].vip = rand() % 100 < cfg.vip_percent;
    orders[v].ndishes = rand_range(1, cfg.max_dishes);
    orders[v].ready = 0;
    orders[v].said_waiting = 0;
    for (int i = 0; i < orders[v].ndishes; i++) {
        orders[v].dish[i].menu = rand() % MENU_SIZE;
        orders[v].dish[i].step = 0;
    }
}

/* Блюда заказа через запятую: «Пицца, Лазанья» */
void dishes_text(int o, char *buf, int size)
{
    buf[0] = '\0';
    for (int i = 0; i < orders[o].ndishes; i++) {
        if (i > 0) strncat(buf, ", ", size - strlen(buf) - 1);
        strncat(buf, MENU[orders[o].dish[i].menu].name, size - strlen(buf) - 1);
    }
}

/* Правило приоритета: 1, если заказ a нужно готовить раньше заказа b */
int order_before(int a, int b)
{
    if (cfg.prio == PRIO_VIP && orders[a].vip != orders[b].vip)
        return orders[a].vip;   /* VIP раньше */
    return a < b;               /* иначе кто раньше заказал */
}

/* Самый приоритетный элемент очереди (заказов кухни или поваров) — его место в очереди */
int best_in_queue(int q)
{
    int best = 0;
    for (int i = 1; i < queue_len[q]; i++) {
        int a = queue[q][i], b = queue[q][best];
        if (q == Q_COOKS) {         /* в очереди поваров лежат блюда, сравниваем их заказы */
            a = ID_ORDER(a);
            b = ID_ORDER(b);
        }
        if (order_before(a, b)) best = i;
    }
    return best;
}

/* Первая свободная единица оборудования eq, -1 — свободных нет */
int free_unit(int eq)
{
    for (int u = 0; u < cfg.equipment[eq]; u++)
        if (equipment_busy[eq][u] == -1) return u;
    return -1;
}

/* Можно ли вообще приготовить заказ. Если нельзя — печатает причину и возвращает 0 */
int check_order(int o)
{
    if (cfg.cooks == 0) {
        log_event(WHO_COORD, 0, "заказ #%d невыполним: на кухне нет поваров. Заказ отменён", o + 1);
        return 0;
    }
    for (int i = 0; i < orders[o].ndishes; i++) {
        int m = orders[o].dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++) {
            int eq = MENU[m].ops[j].eq;
            if (cfg.equipment[eq] == 0) {
                log_event(WHO_COORD, 0, "заказ #%d невыполним: для блюда «%s» нужно оборудование «%s», а его нет. Заказ отменён",
                          o + 1, MENU[m].name, EQ_NAME[eq]);
                return 0;
            }
        }
    }
    if (orders[o].ndishes > cfg.cap_ready) {
        log_event(WHO_COORD, 0, "заказ #%d невыполним: блюд больше, чем мест в зоне готовых блюд (%d > %d). Заказ отменён",
                  o + 1, orders[o].ndishes, cfg.cap_ready);
        return 0;
    }
    return 1;
}

/* Нарушен инвариант. Если программа написана правильно, такого не бывает */
void violation(const char *what, int id)
{
    violations++;
    log_event(WHO_REST, 0, "ОШИБКА: нарушен инвариант — %s (%d)", what, id + 1);
}

/* ===================== Итоги ===================== */

void print_summary(void)
{
    log_text("\n=== Итоги ===");
    log_text("Модельное время: %.1f мин", end_time);
    log_text("Пришло посетителей: %d", n_visitors);
    if (n_served > 0)
        log_text("Обслужено: %d (в среднем %.1f мин в ресторане)", n_served, total_time / n_served);
    else
        log_text("Обслужено: 0");
    log_text("Отменено невыполнимых заказов: %d", n_cancelled);
    log_text("Не обслужено к моменту остановки: %d", n_visitors - n_served - n_cancelled);
    log_text("Приготовлено блюд: %d, выполнено операций: %d", n_dishes_done, n_ops_done);
    for (int c = 0; c < cfg.cooks; c++)
        log_text("  Повар %d: операций — %d, готовил %d мин", c + 1, cook_ops[c], cook_minutes[c]);
    log_text("Нарушений инвариантов: %d", violations);
}
