/*
 * ИДЗ №2, вариант 32. Программа 2: те же потоки и та же логика, что в программе 1,
 * но вместо семафоров — условные переменные.
 *
 * Поток держит мьютекс mutex, пока работает с общими данными, а ждёт через
 * pthread_cond_wait — на время ожидания она отпускает мьютекс. После пробуждения
 * условие всегда проверяется ещё раз в цикле while.
 *
 * Условные переменные:
 *   hall_cv        — официанты ждут работу: посетителя в зале, собранный заказ,
 *                    место в очереди заказов кухни;
 *   kitchen_cv     — координатор ждёт события (флаг kitchen_news);
 *   cook_cv        — повара ждут операцию в очереди;
 *   eq_cv          — повара ждут свободное оборудование;
 *   visitor_cv[v]  — посетитель ждёт официанта, потом свой заказ.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#include "restaurant.h"

static pthread_cond_t hall_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t kitchen_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cook_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t eq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t visitor_cv[MAX_VISITORS];
static int kitchen_news = 0;    /* у координатора есть новые события */

static volatile sig_atomic_t ctrl_c = 0;    /* сколько раз нажали Ctrl+C */

/* Обработчик Ctrl+C: только считаем нажатия, остальное делает главный поток */
static void on_ctrl_c(int sig)
{
    (void)sig;
    ctrl_c++;
}

/* Сообщить координатору о событии (под мьютексом) */
static void tell_coordinator(void)
{
    kitchen_news = 1;
    pthread_cond_signal(&kitchen_cv);
}

/* ===================== Посетитель ===================== */

static void *visitor_thread(void *arg)
{
    int v = (int)(long)arg;

    pthread_mutex_lock(&mutex);
    visitors[v].arrive_time = model_time();
    q_push(Q_HALL, v);
    log_event(WHO_VISITOR, v, "пришёл в ресторан%s и ждёт официанта (ждут в зале: %d)",
              orders[v].vip ? " (VIP)" : "", queue_len[Q_HALL]);
    pthread_cond_broadcast(&hall_cv);       /* зовём официантов */

    while (!stop_now && visitors[v].state == V_WAIT_WAITER)     /* ждём официанта */
        pthread_cond_wait(&visitor_cv[v], &mutex);
    if (!stop_now)
        log_event(WHO_VISITOR, v, "делает заказ официанту %d", visitors[v].waiter + 1);

    while (!stop_now && visitors[v].state != V_SERVED && visitors[v].state != V_CANCELLED)
        pthread_cond_wait(&visitor_cv[v], &mutex);              /* ждём свой заказ или отмену */

    if (visitors[v].state == V_SERVED) {
        double t = model_time() - visitors[v].arrive_time;
        total_time += t;
        log_event(WHO_VISITOR, v, "получил свой заказ #%d и ушёл, провёл в ресторане %.1f мин", v + 1, t);
    } else if (visitors[v].state == V_CANCELLED) {
        log_event(WHO_VISITOR, v, "ушёл без заказа");
    } else {
        log_event(WHO_VISITOR, v, "ушёл, не дождавшись заказа: ресторан закрылся");
    }
    n_left++;
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/* ===================== Официант ===================== */

/* Забрать собранный заказ из зоны готовых блюд (под мьютексом).
   Возвращает номер заказа или -1, если собранных заказов нет */
static int take_ready_order(int w)
{
    if (queue_len[Q_READY] == 0) return -1;
    int o = queue[Q_READY][0];
    q_remove(Q_READY, 0);
    zone_used -= orders[o].ndishes;
    zone_reserved -= orders[o].ndishes;
    log_event(WHO_WAITER, w, "забрал заказ #%d из зоны готовых блюд (%d/%d) и несёт посетителю %d",
              o + 1, zone_used, cfg.cap_ready, o + 1);
    tell_coordinator();                     /* в зоне освободилось место */
    return o;
}

/* Отнести заказ o его посетителю. Вызывается под мьютексом, на время пути он отпускается */
static void deliver(int w, int o)
{
    pthread_mutex_unlock(&mutex);
    sleep_minutes(cfg.deliver_time);        /* несёт заказ */
    pthread_mutex_lock(&mutex);
    if (stop_now) return;

    /* инвариант: заказ выдаётся только после готовности всех блюд и только своему посетителю */
    if (orders[o].ready != orders[o].ndishes || visitors[o].state != V_WAIT_FOOD)
        violation("заказ выдан не готовым или не своему посетителю", o);
    visitors[o].state = V_SERVED;
    n_served++;
    log_event(WHO_WAITER, w, "выдал заказ #%d посетителю %d", o + 1, o + 1);
    pthread_cond_signal(&visitor_cv[o]);
}

static void *waiter_thread(void *arg)
{
    int w = (int)(long)arg;
    char list[400];

    pthread_mutex_lock(&mutex);
    while (1) {
        /* ждём работу: собранный заказ или посетителя в зале */
        while (!stop_now && !finish && queue_len[Q_READY] == 0 && queue_len[Q_HALL] == 0)
            pthread_cond_wait(&hall_cv, &mutex);
        if (stop_now || finish) break;

        /* 1. Сначала разносим собранные заказы */
        int o = take_ready_order(w);
        if (o >= 0) {
            deliver(w, o);
            if (stop_now) break;
            continue;
        }

        /* 2. Подходим к первому посетителю в зале */
        int v = queue[Q_HALL][0];
        q_remove(Q_HALL, 0);
        visitors[v].state = V_ORDERING;
        visitors[v].waiter = w;
        log_event(WHO_WAITER, w, "подошёл к посетителю %d и принимает заказ", v + 1);
        pthread_cond_signal(&visitor_cv[v]);    /* посетитель видит, что к нему подошли */
        pthread_mutex_unlock(&mutex);
        sleep_minutes(cfg.take_time);           /* принимает заказ */
        pthread_mutex_lock(&mutex);
        if (stop_now) break;
        visitors[v].state = V_WAIT_FOOD;
        dishes_text(v, list, sizeof(list));
        log_event(WHO_WAITER, w, "принял заказ #%d у посетителя %d%s: %s",
                  v + 1, v + 1, orders[v].vip ? " (VIP)" : "", list);

        /* 3. Относим заказ на кухню. Если очередь кухни заполнена — ждём у раздачи
              и в это время разносим собранные заказы (иначе зона готовых блюд не
              освободится, координатор не возьмёт заказ из очереди и все встанут) */
        if (queue_len[Q_KITCHEN] >= cfg.cap_orders)
            log_event(WHO_WAITER, w, "очередь заказов кухни заполнена (%d/%d), ждёт у раздачи с заказом #%d",
                      queue_len[Q_KITCHEN], cfg.cap_orders, v + 1);
        while (!stop_now && queue_len[Q_KITCHEN] >= cfg.cap_orders) {
            int r = take_ready_order(w);
            if (r >= 0)
                deliver(w, r);
            else
                pthread_cond_wait(&hall_cv, &mutex);
        }
        if (stop_now) break;
        q_push(Q_KITCHEN, v);
        log_event(WHO_WAITER, w, "передал заказ #%d на кухню (очередь заказов %d/%d)",
                  v + 1, queue_len[Q_KITCHEN], cfg.cap_orders);
        tell_coordinator();                     /* у координатора новый заказ */
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/* ===================== Координатор кухни ===================== */

/* Взять заказ в работу: создать операции и поставить первые операции блюд в очередь поваров */
static void start_order(int o)
{
    zone_reserved += orders[o].ndishes;     /* бронируем места в зоне готовых блюд */
    log_event(WHO_COORD, 0, "заказ #%d%s взят в работу, в зоне готовых блюд забронировано %d/%d мест",
              o + 1, orders[o].vip ? " (VIP)" : "", zone_reserved, cfg.cap_ready);

    for (int i = 0; i < orders[o].ndishes; i++) {
        int m = orders[o].dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++)
            log_event(WHO_COORD, 0, "  создана операция: блюдо %d «%s», шаг %d — %s (%s, %d мин)",
                      i + 1, MENU[m].name, j + 1, MENU[m].ops[j].name,
                      EQ_NAME[MENU[m].ops[j].eq], MENU[m].ops[j].time);
        q_push(Q_COOKS, DISH_ID(o, i));
        log_event(WHO_COORD, 0, "  «%s» (заказ #%d, блюдо %d: %s) — в очередь поваров",
                  MENU[m].ops[0].name, o + 1, i + 1, MENU[m].name);
        pthread_cond_signal(&cook_cv);
    }
}

/* Повар закончил текущую операцию блюда id */
static void operation_done(int id)
{
    int o = ID_ORDER(id), d = ID_DISH(id);
    int m = orders[o].dish[d].menu;

    orders[o].dish[d].step++;
    int s = orders[o].dish[d].step;
    if (s < MENU[m].nops) {
        /* следующая операция блюда попадает в очередь только теперь, после предыдущей */
        q_push(Q_COOKS, id);
        log_event(WHO_COORD, 0, "«%s» (заказ #%d, блюдо %d: %s) — в очередь поваров",
                  MENU[m].ops[s].name, o + 1, d + 1, MENU[m].name);
        pthread_cond_signal(&cook_cv);
        return;
    }

    /* все операции выполнены — блюдо готово */
    zone_used++;
    orders[o].ready++;
    n_dishes_done++;
    log_event(WHO_COORD, 0, "блюдо %d заказа #%d «%s» готово → зона готовых блюд (%d/%d); сборка заказа #%d: %d из %d блюд",
              d + 1, o + 1, MENU[m].name, zone_used, cfg.cap_ready, o + 1, orders[o].ready, orders[o].ndishes);
    if (orders[o].ready == orders[o].ndishes) {
        log_event(WHO_COORD, 0, "заказ #%d собран и ждёт официанта", o + 1);
        q_push(Q_READY, o);
        pthread_cond_broadcast(&hall_cv);   /* работа для официантов */
    }
}

static void *coordinator_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mutex);
    while (1) {
        while (!stop_now && !finish && !kitchen_news)   /* ждём событие */
            pthread_cond_wait(&kitchen_cv, &mutex);
        if (stop_now || finish) break;
        kitchen_news = 0;

        /* 1. Сообщения поваров о законченных операциях */
        while (queue_len[Q_DONE] > 0) {
            int id = queue[Q_DONE][0];
            q_remove(Q_DONE, 0);
            operation_done(id);
        }

        /* 2. Заказы из очереди кухни — по правилу приоритета, пока есть место в зоне готовых блюд.
              Места бронируются сразу под все блюда заказа: иначе зона может забиться
              блюдами разных недоделанных заказов, и ни один заказ не соберётся */
        while (queue_len[Q_KITCHEN] > 0) {
            int i = best_in_queue(Q_KITCHEN);
            int o = queue[Q_KITCHEN][i];
            if (!check_order(o)) {          /* невыполнимый заказ отменяем */
                q_remove(Q_KITCHEN, i);
                visitors[o].state = V_CANCELLED;
                n_cancelled++;
                pthread_cond_signal(&visitor_cv[o]);
                pthread_cond_broadcast(&hall_cv);   /* в очереди кухни освободилось место */
                continue;
            }
            if (zone_reserved + orders[o].ndishes > cfg.cap_ready) {
                if (!orders[o].said_waiting) {
                    orders[o].said_waiting = 1;
                    log_event(WHO_COORD, 0, "заказ #%d ждёт: в зоне готовых блюд нет места (%d + %d > %d)",
                              o + 1, zone_reserved, orders[o].ndishes, cfg.cap_ready);
                }
                break;
            }
            q_remove(Q_KITCHEN, i);
            pthread_cond_broadcast(&hall_cv);       /* в очереди кухни освободилось место */
            start_order(o);
        }
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/* ===================== Повар ===================== */

static void *cook_thread(void *arg)
{
    int c = (int)(long)arg;

    pthread_mutex_lock(&mutex);
    while (1) {
        while (!stop_now && !finish && queue_len[Q_COOKS] == 0)    /* ждём операцию */
            pthread_cond_wait(&cook_cv, &mutex);
        if (stop_now || finish) break;

        /* самая приоритетная операция из очереди */
        int i = best_in_queue(Q_COOKS);
        int id = queue[Q_COOKS][i];
        q_remove(Q_COOKS, i);
        int o = ID_ORDER(id), d = ID_DISH(id);
        int m = orders[o].dish[d].menu;
        int s = orders[o].dish[d].step;
        Operation op = MENU[m].ops[s];
        log_event(WHO_COOK, c, "взял из очереди «%s» (заказ #%d, блюдо %d: %s)", op.name, o + 1, d + 1, MENU[m].name);

        /* занимаем единицу нужного оборудования; если все заняты — ждём */
        if (free_unit(op.eq) < 0) {
            log_event(WHO_COOK, c, "ждёт, когда освободится %s", EQ_NAME[op.eq]);
            while (!stop_now && free_unit(op.eq) < 0)
                pthread_cond_wait(&eq_cv, &mutex);
            if (stop_now) break;
        }
        int u = free_unit(op.eq);
        equipment_busy[op.eq][u] = c;       /* теперь эта единица занята этим поваром */
        log_event(WHO_COOK, c, "получил %s №%d и начал «%s» (заказ #%d, блюдо %d: %s), шаг %d/%d — %d мин",
                  EQ_ACC[op.eq], u + 1, op.name, o + 1, d + 1, MENU[m].name, s + 1, MENU[m].nops, op.time);
        pthread_mutex_unlock(&mutex);

        sleep_minutes(op.time);             /* готовит */

        pthread_mutex_lock(&mutex);
        if (equipment_busy[op.eq][u] != c)
            violation("оборудование во время готовки занял другой повар", c);
        equipment_busy[op.eq][u] = -1;
        cook_ops[c]++;
        cook_minutes[c] += op.time;
        n_ops_done++;
        log_event(WHO_COOK, c, "закончил «%s» (заказ #%d, блюдо %d: %s) и освободил %s №%d",
                  op.name, o + 1, d + 1, MENU[m].name, EQ_ACC[op.eq], u + 1);
        q_push(Q_DONE, id);                 /* сообщение координатору */
        tell_coordinator();
        pthread_cond_broadcast(&eq_cv);     /* оборудование освободилось — может, его кто-то ждёт */
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/* ===================== Главный поток ===================== */

/* Немедленная остановка: будим всех ждущих, каждый увидит stop_now и закончит работу */
static void stop_all(void)
{
    pthread_mutex_lock(&mutex);
    stop_now = 1;
    log_event(WHO_REST, 0, "получен Ctrl+C — немедленная остановка");
    pthread_cond_broadcast(&hall_cv);
    pthread_cond_broadcast(&kitchen_cv);
    pthread_cond_broadcast(&cook_cv);
    pthread_cond_broadcast(&eq_cv);
    for (int v = 0; v < n_visitors; v++) pthread_cond_signal(&visitor_cv[v]);
    pthread_mutex_unlock(&mutex);
}

int main(void)
{
    static pthread_t visitor_tid[MAX_VISITORS];
    pthread_t coord_tid, cook_tid[MAX_STAFF], waiter_tid[MAX_STAFF];
    sigset_t sigint;

    srand((unsigned)time(NULL));
    read_config();
    open_log("restaurant_cv.log");
    log_text("\n=== Кухня ресторана: потоки, мьютекс и условные переменные ===");
    print_config();

    for (int t = 0; t < EQ_TYPES; t++)
        for (int u = 0; u < MAX_UNITS; u++) equipment_busy[t][u] = -1;

    /* Ctrl+C — как в программе 1: обработчик через sigaction, сигнал получает
       только главный поток (на время создания потоков он заблокирован) */
    struct sigaction sa;
    sa.sa_handler = on_ctrl_c;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigemptyset(&sigint);
    sigaddset(&sigint, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigint, NULL);
    start_clock();
    pthread_create(&coord_tid, NULL, coordinator_thread, NULL);
    for (int c = 0; c < cfg.cooks; c++) pthread_create(&cook_tid[c], NULL, cook_thread, (void *)(long)c);
    for (int w = 0; w < cfg.waiters; w++) pthread_create(&waiter_tid[w], NULL, waiter_thread, (void *)(long)w);
    pthread_sigmask(SIG_UNBLOCK, &sigint, NULL);

    if (cfg.visitors == 0)
        log_text("Режим без ограничения времени: посетители приходят, пока не нажать Ctrl+C");
    log_text("Ctrl+C: если вход открыт — закрыть его и дообслужить тех, кто внутри; если уже закрыт — остановить сразу\n");
    pthread_mutex_lock(&mutex);
    log_event(WHO_REST, 0, "ресторан открыт");
    pthread_mutex_unlock(&mutex);

    /* Вход: впускаем посетителей, пока вход не закроют */
    int used = 0;                           /* сколько нажатий Ctrl+C уже учтено */
    for (int v = 0; ; v++) {
        if (ctrl_c > 0) {
            used = 1;
            pthread_mutex_lock(&mutex);
            log_event(WHO_REST, 0, "получен Ctrl+C — вход закрыт, дообслуживаем тех, кто внутри");
            pthread_mutex_unlock(&mutex);
            break;
        }
        make_order(v);                      /* посетитель сразу решает, что закажет */
        pthread_cond_init(&visitor_cv[v], NULL);
        pthread_mutex_lock(&mutex);
        n_visitors = v + 1;
        pthread_mutex_unlock(&mutex);
        pthread_sigmask(SIG_BLOCK, &sigint, NULL);
        pthread_create(&visitor_tid[v], NULL, visitor_thread, (void *)(long)v);
        pthread_sigmask(SIG_UNBLOCK, &sigint, NULL);

        if (cfg.visitors > 0 && v + 1 == cfg.visitors) {
            pthread_mutex_lock(&mutex);
            log_event(WHO_REST, 0, "вход закрыт: пришли все %d посетителей", cfg.visitors);
            pthread_mutex_unlock(&mutex);
            break;
        }
        if (v + 1 == MAX_VISITORS) {
            pthread_mutex_lock(&mutex);
            log_event(WHO_REST, 0, "вход закрыт: в модели не может быть больше %d посетителей", MAX_VISITORS);
            pthread_mutex_unlock(&mutex);
            break;
        }
        sleep_minutes(rand_range(cfg.arrive_min, cfg.arrive_max));   /* Ctrl+C прервёт паузу */
    }

    /* Ждём, пока уйдут все посетители: раз в 100 мс смотрим на счётчик ушедших.
       Ctrl+C прерывает паузу, и главный поток сразу проверяет, не пора ли остановиться */
    int stopped = 0;
    while (1) {
        pthread_mutex_lock(&mutex);
        int all_left = (n_left == n_visitors);
        pthread_mutex_unlock(&mutex);
        if (all_left) break;
        if (!stopped && ctrl_c > used) {
            stopped = 1;
            stop_all();
        }
        sleep_ms(100);
    }
    for (int v = 0; v < n_visitors; v++) pthread_join(visitor_tid[v], NULL);

    /* Посетителей больше нет — сотрудники могут расходиться */
    pthread_mutex_lock(&mutex);
    end_time = model_time();
    if (stop_now)
        log_event(WHO_REST, 0, "моделирование остановлено");
    else
        log_event(WHO_REST, 0, "все посетители ушли — ресторан закрывается");
    finish = 1;
    pthread_cond_broadcast(&hall_cv);
    pthread_cond_broadcast(&kitchen_cv);
    pthread_cond_broadcast(&cook_cv);
    pthread_mutex_unlock(&mutex);

    pthread_join(coord_tid, NULL);
    for (int c = 0; c < cfg.cooks; c++) pthread_join(cook_tid[c], NULL);
    for (int w = 0; w < cfg.waiters; w++) pthread_join(waiter_tid[w], NULL);

    print_summary();
    log_text("Журнал сохранён в файл restaurant_cv.log");
    close_log();

    for (int v = 0; v < n_visitors; v++) pthread_cond_destroy(&visitor_cv[v]);
    return 0;
}
