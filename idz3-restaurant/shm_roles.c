/* Логика та же, что в ИДЗ-2 (версия на семафорах), только данные в разделяемой памяти */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>

#include "shm.h"

static void violation(const char *what, int id)
{
    sh->violations++;
    log_event(WHO_REST, 0, "ОШИБКА: нарушен инвариант — %s (%d)", what, id + 1);
}

static void q_push(int q, int x)
{
    if (sh->queue_len[q] < QUEUE_MAX) {
        sh->queue[q][sh->queue_len[q]] = x;
        sh->queue_len[q]++;
    }
}

static void q_remove(int q, int i)
{
    for (int k = i; k < sh->queue_len[q] - 1; k++)
        sh->queue[q][k] = sh->queue[q][k + 1];
    sh->queue_len[q]--;
}

static int best_in_queue(int q)
{
    int best = 0;
    for (int i = 1; i < sh->queue_len[q]; i++) {
        int a = sh->queue[q][i], b = sh->queue[q][best];
        if (q == Q_COOKS) {
            a = ID_ORDER(a);
            b = ID_ORDER(b);
        }
        if (order_before(a, sh->orders[a].vip, b, sh->orders[b].vip)) best = i;
    }
    return best;
}

void visitor_role(int v)
{
    P(s_mutex);
    sh->visitors[v].arrive_time = model_time();
    q_push(Q_HALL, v);
    log_event(WHO_VISITOR, v, "пришёл в ресторан%s и ждёт официанта (ждут в зале: %d)",
              sh->orders[v].vip ? " (VIP)" : "", sh->queue_len[Q_HALL]);
    V(s_mutex);
    V(s_waiter_work);

    P(visitor_sem(v));
    P(s_mutex);
    if (!sh->stop_now)
        log_event(WHO_VISITOR, v, "делает заказ официанту %d", sh->visitors[v].waiter + 1);
    V(s_mutex);

    P(visitor_sem(v));
    P(s_mutex);
    if (sh->visitors[v].state == V_SERVED) {
        double t = model_time() - sh->visitors[v].arrive_time;
        sh->total_time += t;
        log_event(WHO_VISITOR, v, "получил свой заказ #%d и ушёл, провёл в ресторане %.1f мин", v + 1, t);
    } else if (sh->visitors[v].state == V_CANCELLED) {
        log_event(WHO_VISITOR, v, "ушёл без заказа");
    } else {
        log_event(WHO_VISITOR, v, "ушёл, не дождавшись заказа: ресторан закрылся");
    }
    sh->n_left++;
    V(s_mutex);
    V(s_left);
}

int new_visitor(void)
{
    P(s_mutex);
    int v = sh->n_visitors;
    make_order(&sh->orders[v]);
    sh->visitors[v].state = V_WAIT_WAITER;
    sh->visitors[v].waiter = -1;
    create_visitor_sem(v);
    sh->n_visitors = v + 1;
    V(s_mutex);
    return v;
}

static int take_ready_order(int w)
{
    if (sh->queue_len[Q_READY] == 0) return -1;
    int o = sh->queue[Q_READY][0];
    q_remove(Q_READY, 0);
    sh->zone_used -= sh->orders[o].ndishes;
    sh->zone_reserved -= sh->orders[o].ndishes;
    log_event(WHO_WAITER, w, "забрал заказ #%d из зоны готовых блюд (%d/%d) и несёт посетителю %d",
              o + 1, sh->zone_used, cfg.cap_ready, o + 1);
    return o;
}

static void deliver(int w, int o)
{
    V(s_coord);
    sleep_minutes(cfg.deliver_time);

    P(s_mutex);
    if (sh->stop_now) {
        V(s_mutex);
        return;
    }
    if (sh->orders[o].ready != sh->orders[o].ndishes || sh->visitors[o].state != V_WAIT_FOOD)
        violation("заказ выдан не готовым или не своему посетителю", o);
    sh->visitors[o].state = V_SERVED;
    sh->n_served++;
    sh->waiter_delivered[w]++;
    log_event(WHO_WAITER, w, "выдал заказ #%d посетителю %d", o + 1, o + 1);
    V(s_mutex);
    V(visitor_sem(o));
}

/* Отдать заказ v на кухню. Если очередь кухни заполнена — ждать у раздачи и пока разносить
   собранные заказы. Иначе может случиться так, что все официанты стоят у раздачи, зону
   готовых блюд никто не освобождает, координатор не берёт заказы — и все встали */
static void give_to_kitchen(int w, int v)
{
    if (sem_trywait(s_kitchen_free) != 0) {
        P(s_mutex);
        log_event(WHO_WAITER, w, "очередь заказов кухни заполнена (%d/%d), ждёт у раздачи с заказом #%d",
                  sh->queue_len[Q_KITCHEN], cfg.cap_orders, v + 1);
        V(s_mutex);
        while (sem_trywait(s_kitchen_free) != 0) {
            P(s_mutex);
            if (sh->stop_now) {
                V(s_mutex);
                return;
            }
            int o = take_ready_order(w);
            V(s_mutex);
            if (o >= 0)
                deliver(w, o);
            else
                sleep_minutes(1);
        }
    }
    P(s_mutex);
    q_push(Q_KITCHEN, v);
    log_event(WHO_WAITER, w, "передал заказ #%d на кухню (очередь заказов %d/%d)",
              v + 1, sh->queue_len[Q_KITCHEN], cfg.cap_orders);
    V(s_mutex);
    V(s_coord);
}

void waiter_role(int w)
{
    char list[400];

    while (1) {
        P(s_waiter_work);
        P(s_mutex);
        if (sh->stop_now || sh->finish) {
            V(s_mutex);
            break;
        }
        int o = take_ready_order(w);
        if (o >= 0) {
            V(s_mutex);
            deliver(w, o);
            continue;
        }
        if (sh->queue_len[Q_HALL] == 0) {
            V(s_mutex);
            continue;
        }
        int v = sh->queue[Q_HALL][0];
        q_remove(Q_HALL, 0);
        sh->visitors[v].state = V_ORDERING;
        sh->visitors[v].waiter = w;
        log_event(WHO_WAITER, w, "подошёл к посетителю %d и принимает заказ", v + 1);
        V(s_mutex);
        V(visitor_sem(v));

        sleep_minutes(cfg.take_time);

        P(s_mutex);
        if (sh->stop_now) {
            V(s_mutex);
            break;
        }
        sh->visitors[v].state = V_WAIT_FOOD;
        sh->waiter_taken[w]++;
        dishes_text(&sh->orders[v], list, sizeof(list));
        log_event(WHO_WAITER, w, "принял заказ #%d у посетителя %d%s: %s",
                  v + 1, v + 1, sh->orders[v].vip ? " (VIP)" : "", list);
        V(s_mutex);

        give_to_kitchen(w, v);
    }
}

static void start_order(int o)
{
    Order *ord = &sh->orders[o];
    sh->zone_reserved += ord->ndishes;
    log_event(WHO_COORD, 0, "заказ #%d%s взят в работу, в зоне готовых блюд забронировано %d/%d мест",
              o + 1, ord->vip ? " (VIP)" : "", sh->zone_reserved, cfg.cap_ready);

    for (int i = 0; i < ord->ndishes; i++) {
        int m = ord->dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++)
            log_event(WHO_COORD, 0, "  создана операция: блюдо %d «%s», шаг %d — %s (%s, %d мин)",
                      i + 1, MENU[m].name, j + 1, MENU[m].ops[j].name,
                      EQ_NAME[MENU[m].ops[j].eq], MENU[m].ops[j].time);
        q_push(Q_COOKS, DISH_ID(o, i));
        log_event(WHO_COORD, 0, "  «%s» (заказ #%d, блюдо %d: %s) — в очередь поваров",
                  MENU[m].ops[0].name, o + 1, i + 1, MENU[m].name);
        V(s_cook_work);
    }
}

static void operation_done(int id)
{
    int o = ID_ORDER(id), d = ID_DISH(id);
    Order *ord = &sh->orders[o];
    int m = ord->dish[d].menu;

    ord->dish[d].step++;
    int s = ord->dish[d].step;
    if (s < MENU[m].nops) {
        q_push(Q_COOKS, id);
        log_event(WHO_COORD, 0, "«%s» (заказ #%d, блюдо %d: %s) — в очередь поваров",
                  MENU[m].ops[s].name, o + 1, d + 1, MENU[m].name);
        V(s_cook_work);
        return;
    }
    sh->zone_used++;
    ord->ready++;
    sh->n_dishes_done++;
    log_event(WHO_COORD, 0, "блюдо %d заказа #%d «%s» готово → зона готовых блюд (%d/%d); сборка заказа #%d: %d из %d блюд",
              d + 1, o + 1, MENU[m].name, sh->zone_used, cfg.cap_ready, o + 1, ord->ready, ord->ndishes);
    if (ord->ready == ord->ndishes) {
        log_event(WHO_COORD, 0, "заказ #%d собран и ждёт официанта", o + 1);
        q_push(Q_READY, o);
        V(s_waiter_work);
    }
}

int coordinator_step(void)
{
    P(s_mutex);
    if (sh->stop_now || sh->finish) {
        V(s_mutex);
        return 0;
    }
    while (sh->queue_len[Q_DONE] > 0) {
        int id = sh->queue[Q_DONE][0];
        q_remove(Q_DONE, 0);
        operation_done(id);
    }
    /* Заказы из очереди кухни — по правилу приоритета, пока есть место в зоне готовых блюд.
          Места бронируются сразу под все блюда заказа: иначе зона может забиться
          блюдами разных недоделанных заказов, и ни один заказ не соберётся */
    while (sh->queue_len[Q_KITCHEN] > 0) {
        int i = best_in_queue(Q_KITCHEN);
        int o = sh->queue[Q_KITCHEN][i];
        if (!check_order(&sh->orders[o], o)) {
            q_remove(Q_KITCHEN, i);
            sh->visitors[o].state = V_CANCELLED;
            sh->n_cancelled++;
            V(s_kitchen_free);
            V(visitor_sem(o));
            continue;
        }
        if (sh->zone_reserved + sh->orders[o].ndishes > cfg.cap_ready) {
            if (!sh->orders[o].said_waiting) {
                sh->orders[o].said_waiting = 1;
                log_event(WHO_COORD, 0, "заказ #%d ждёт: в зоне готовых блюд нет места (%d + %d > %d)",
                          o + 1, sh->zone_reserved, sh->orders[o].ndishes, cfg.cap_ready);
            }
            break;
        }
        q_remove(Q_KITCHEN, i);
        V(s_kitchen_free);
        start_order(o);
    }
    V(s_mutex);
    return 1;
}

void coordinator_role(void)
{
    while (1) {
        P(s_coord);
        if (!coordinator_step()) break;
    }
}

void cook_role(int c)
{
    while (1) {
        P(s_cook_work);
        P(s_mutex);
        if (sh->stop_now || sh->finish) {
            V(s_mutex);
            break;
        }
        if (sh->queue_len[Q_COOKS] == 0) {
            V(s_mutex);
            continue;
        }
        int i = best_in_queue(Q_COOKS);
        int id = sh->queue[Q_COOKS][i];
        q_remove(Q_COOKS, i);
        int o = ID_ORDER(id), d = ID_DISH(id);
        int m = sh->orders[o].dish[d].menu;
        int s = sh->orders[o].dish[d].step;
        Operation op = MENU[m].ops[s];
        log_event(WHO_COOK, c, "взял из очереди «%s» (заказ #%d, блюдо %d: %s)", op.name, o + 1, d + 1, MENU[m].name);
        V(s_mutex);

        if (sem_trywait(s_eq[op.eq]) != 0) {
            P(s_mutex);
            log_event(WHO_COOK, c, "ждёт, когда освободится %s", EQ_NAME[op.eq]);
            V(s_mutex);
            P(s_eq[op.eq]);
        }
        P(s_mutex);
        if (sh->stop_now) {
            V(s_mutex);
            break;
        }
        int u = 0;
        while (u < cfg.equipment[op.eq] && sh->equipment_busy[op.eq][u] != -1) u++;
        if (u == cfg.equipment[op.eq]) {
            violation("нет свободной единицы оборудования", c);
            u = 0;
        }
        sh->equipment_busy[op.eq][u] = c;
        log_event(WHO_COOK, c, "получил %s №%d и начал «%s» (заказ #%d, блюдо %d: %s), шаг %d/%d — %d мин",
                  EQ_ACC[op.eq], u + 1, op.name, o + 1, d + 1, MENU[m].name, s + 1, MENU[m].nops, op.time);
        V(s_mutex);

        sleep_minutes(op.time);

        P(s_mutex);
        if (sh->equipment_busy[op.eq][u] != c)
            violation("оборудование во время готовки занял другой повар", c);
        sh->equipment_busy[op.eq][u] = -1;
        sh->cook_ops[c]++;
        sh->cook_minutes[c] += op.time;
        sh->n_ops_done++;
        log_event(WHO_COOK, c, "закончил «%s» (заказ #%d, блюдо %d: %s) и освободил %s №%d",
                  op.name, o + 1, d + 1, MENU[m].name, EQ_ACC[op.eq], u + 1);
        q_push(Q_DONE, id);
        V(s_mutex);
        V(s_eq[op.eq]);
        V(s_coord);
    }
}

void close_intake(const char *why)
{
    P(s_mutex);
    if (!sh->intake_closed && !sh->stop_now) {
        sh->intake_closed = 1;
        log_event(WHO_REST, 0, "%s", why);
    }
    V(s_mutex);
    if (s_door != NULL) V(s_door);
}

/* Немедленная остановка: будим всех, кто может ждать на семафорах.
   Каждый процесс после пробуждения видит stop_now и заканчивает работу */
void stop_all(const char *why)
{
    P(s_mutex);
    if (sh->stop_now || sh->finish) {
        V(s_mutex);
        return;
    }
    sh->stop_now = 1;
    sh->intake_closed = 1;
    sh->end_time = model_time();
    log_event(WHO_REST, 0, "%s — немедленная остановка", why);
    int n = sh->n_visitors;
    V(s_mutex);

    for (int w = 0; w < cfg.waiters; w++) V(s_waiter_work);
    V(s_coord);
    for (int c = 0; c < cfg.cooks; c++) {
        V(s_cook_work);
        for (int t = 0; t < EQ_TYPES; t++) V(s_eq[t]);
    }
    for (int v = 0; v < n; v++) {
        V(visitor_sem(v));
        V(visitor_sem(v));
    }
    if (s_door != NULL) V(s_door);
    if (s_staff != NULL) V(s_staff);
}

void finish_all(void)
{
    P(s_mutex);
    sh->end_time = model_time();
    if (!sh->stop_now)
        log_event(WHO_REST, 0, "все посетители ушли — ресторан закрывается");
    else
        log_event(WHO_REST, 0, "моделирование остановлено");
    sh->finish = 1;
    V(s_mutex);
    for (int w = 0; w < cfg.waiters; w++) V(s_waiter_work);
    V(s_coord);
    for (int c = 0; c < cfg.cooks; c++) V(s_cook_work);
}

void print_summary(void)
{
    int n = sh->n_visitors;
    log_text("\n=== Итоги ===");
    log_text("Модельное время: %.1f мин", sh->end_time);
    log_text("Пришло посетителей: %d", n);
    if (sh->n_served > 0)
        log_text("Обслужено: %d (в среднем %.1f мин в ресторане)", sh->n_served, sh->total_time / sh->n_served);
    else
        log_text("Обслужено: 0");
    log_text("Отменено невыполнимых заказов: %d", sh->n_cancelled);
    log_text("Не обслужено к моменту остановки: %d", n - sh->n_served - sh->n_cancelled);
    log_text("Приготовлено блюд: %d, выполнено операций: %d", sh->n_dishes_done, sh->n_ops_done);
    for (int w = 0; w < cfg.waiters; w++)
        log_text("  Официант %d: принял заказов — %d, выдал — %d", w + 1, sh->waiter_taken[w], sh->waiter_delivered[w]);
    for (int c = 0; c < cfg.cooks; c++)
        log_text("  Повар %d: операций — %d, готовил %d мин", c + 1, sh->cook_ops[c], sh->cook_minutes[c]);
    log_text("Нарушений инвариантов: %d", sh->violations);
}
