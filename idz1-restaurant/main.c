/*
 * ИДЗ №1, вариант 32 «Кухня ресторана».
 *
 * Последовательная имитация в одном процессе и одном потоке.
 * Модельное время идёт в минутах. Всё, что должно случиться в будущем
 * (повар закончит операцию, официант донесёт заказ, придёт посетитель),
 * записывается в список событий. Главный цикл берёт самое раннее событие,
 * переводит часы на его время и обрабатывает. После этого все свободные
 * участники берут себе новую работу.
 *
 * Файлы:
 *   restaurant.h — типы и общие переменные
 *   main.c       — главный цикл, Ctrl+C, итоги
 *   config.c     — меню, сценарии, ввод параметров
 *   events.c     — список событий
 *   hall.c       — посетители и официанты
 *   kitchen.c    — координатор кухни и повара
 *   util.c       — вывод, случайные числа, очереди
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "restaurant.h"

/* ---------- Состояние модели ---------- */
long now;
Visitor visitors[MAX_VISITORS];
Order orders[MAX_VISITORS];
int n_visitors;
Waiter waiters[MAX_STAFF];
Cook cooks[MAX_STAFF];
int equipment_busy[EQ_TYPES][MAX_UNITS];
int queue[NQUEUES][QUEUE_MAX];
int queue_len[NQUEUES];
int zone_used, zone_reserved;
int intake_closed, n_served, n_cancelled;
long total_wait;
volatile sig_atomic_t stop_flag = 0;

/* Обработчик Ctrl+C: только ставим флаг, остановится главный цикл */
static void on_ctrl_c(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* Все участники по очереди берут работу, пока хоть что-то меняется */
static void everyone_works(void)
{
    int changed;
    do {
        changed = 0;
        changed |= waiters_work();
        changed |= coordinator_work();
        changed |= cooks_work();
    } while (changed);
}

static void run(void)
{
    log_who(WHO_REST, 0);
    printf("ресторан открыт\n");
    add_event(0, EV_ARRIVAL, 0);

    while (1) {
        if (stop_flag) {
            log_who(WHO_REST, 0);
            printf("получен сигнал прерывания (Ctrl+C), моделирование остановлено\n");
            break;
        }
        if (intake_closed && n_served + n_cancelled == n_visitors) {
            log_who(WHO_REST, 0);
            printf("все посетители обслужены — ресторан закрывается\n");
            break;
        }
        if (no_events()) {
            log_who(WHO_REST, 0);
            printf("событий больше нет, моделирование остановлено\n");
            break;
        }
        Event e = next_event();
        if (e.time > now) {
            sleep_ms((e.time - now) * cfg.delay_ms);   /* пауза, чтобы успеть прочитать */
            now = e.time;
        }
        switch (e.type) {
        case EV_ARRIVAL:     visitor_arrives();      break;
        case EV_ORDER_TAKEN: order_taken(e.who);     break;
        case EV_COOK_DONE:   cook_done(e.who);       break;
        case EV_DELIVERED:   order_delivered(e.who); break;
        }
        everyone_works();
    }
}

static void print_summary(void)
{
    printf("\n=== Итоги ===\n");
    printf("Модельное время: %ld мин\n", now);
    printf("Пришло посетителей: %d\n", n_visitors);
    printf("Обслужено: %d", n_served);
    if (n_served > 0) printf(" (в среднем %.1f мин в ресторане)", (double)total_wait / n_served);
    printf("\nОтменено невыполнимых заказов: %d\n", n_cancelled);
    printf("Не обслужено к моменту остановки: %d\n", n_visitors - n_served - n_cancelled);
}

int main(void)
{
    srand((unsigned)time(NULL));

    printf("=== Кухня ресторана: имитационная модель ===\n\nСценарии:\n");
    for (int i = 0; i < NPRESETS; i++) printf("  %d — %s\n", i + 1, PRESETS[i].title);
    int p = ask_int("Выберите сценарий", 1, 1, NPRESETS);
    cfg = PRESETS[p - 1];
    if (ask_yes("Изменить параметры сценария?")) {
        edit_config();
        cfg.title = "свои параметры";
    }
    if (cfg.visitors == 0 && cfg.delay_ms < 10) cfg.delay_ms = 10;   /* чтобы вывод успевал читаться */
    print_config();

    /* в начале все официанты, повара и оборудование свободны */
    for (int w = 0; w < cfg.waiters; w++) {
        waiters[w].state = W_FREE;
        waiters[w].visitor = waiters[w].slip = waiters[w].carrying = -1;
    }
    for (int eq = 0; eq < EQ_TYPES; eq++)
        for (int u = 0; u < MAX_UNITS; u++)
            equipment_busy[eq][u] = -1;

    /* два варианта завершения в любом режиме: само или по Ctrl+C */
    signal(SIGINT, on_ctrl_c);
    if (cfg.visitors == 0)
        printf("Режим без ограничения времени: для остановки нажмите Ctrl+C\n\n");
    else
        printf("Моделирование закончится, когда все посетители будут обслужены; прервать — Ctrl+C\n\n");

    run();
    print_summary();
    return 0;
}
