/* Кухня: координатор и повара */
#include <stdio.h>

#include "restaurant.h"

/* Блюдо в очереди поваров хранится одним числом: заказ * MAX_DISHES + номер блюда.
   Например, блюдо 2 заказа 5 — это 5 * 8 + 2 = 42 */
#define DISH_ID(o, d)   ((o) * MAX_DISHES + (d))
#define ID_ORDER(id)    ((id) / MAX_DISHES)
#define ID_DISH(id)     ((id) % MAX_DISHES)

/* Правило приоритета: 1, если заказ a нужно готовить раньше заказа b */
int order_before(int a, int b)
{
    if (cfg.prio == PRIO_VIP && orders[a].vip != orders[b].vip)
        return orders[a].vip;   /* VIP раньше */
    return a < b;               /* иначе кто раньше заказал */
}

/* ===================== Координатор кухни ===================== */

/* Можно ли вообще приготовить заказ. Если нельзя — печатает причину и возвращает 0 */
int check_order(int o)
{
    if (cfg.cooks == 0) {
        log_who(WHO_COORD, 0);
        printf("заказ #%d невыполним: на кухне нет поваров. Заказ отменён\n", o + 1);
        return 0;
    }
    for (int i = 0; i < orders[o].ndishes; i++) {
        int m = orders[o].dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++) {
            int eq = MENU[m].ops[j].eq;
            if (cfg.equipment[eq] == 0) {
                log_who(WHO_COORD, 0);
                printf("заказ #%d невыполним: для блюда «%s» нужно оборудование «%s», а его нет. Заказ отменён\n",
                       o + 1, MENU[m].name, EQ_NAME[eq]);
                return 0;
            }
        }
    }
    if (orders[o].ndishes > cfg.cap_ready) {
        log_who(WHO_COORD, 0);
        printf("заказ #%d невыполним: блюд больше, чем мест в зоне готовых блюд (%d > %d). Заказ отменён\n",
               o + 1, orders[o].ndishes, cfg.cap_ready);
        return 0;
    }
    return 1;
}

/* Взять заказ в работу: создать операции и поставить блюда в очередь поваров */
static void start_order(int o)
{
    zone_reserved += orders[o].ndishes;   /* бронируем места в зоне готовых блюд */
    log_who(WHO_COORD, 0);
    printf("заказ #%d%s взят в работу, в зоне готовых блюд забронировано %d/%d мест\n",
           o + 1, orders[o].vip ? " (VIP)" : "", zone_reserved, cfg.cap_ready);

    for (int i = 0; i < orders[o].ndishes; i++) {
        int m = orders[o].dish[i].menu;
        for (int j = 0; j < MENU[m].nops; j++) {
            log_who(WHO_COORD, 0);
            printf("  создана операция: %s, шаг %d — %s (%s, %d мин)\n",
                   MENU[m].name, j + 1, MENU[m].ops[j].name, EQ_NAME[MENU[m].ops[j].eq], MENU[m].ops[j].time);
        }
        q_push(Q_COOKS, DISH_ID(o, i));
        log_who(WHO_COORD, 0);
        printf("  «%s» (%s, заказ #%d) — в очередь поваров\n", MENU[m].ops[0].name, MENU[m].name, o + 1);
    }
}

/* Координатор берёт заказы из очереди кухни, пока есть место в зоне готовых блюд.
   Бронь мест нужна, чтобы зона не забилась блюдами недоделанных заказов */
int coordinator_work(void)
{
    int changed = 0;
    while (queue_len[Q_KITCHEN] > 0) {
        int best = 0;
        for (int i = 1; i < queue_len[Q_KITCHEN]; i++)
            if (order_before(queue[Q_KITCHEN][i], queue[Q_KITCHEN][best])) best = i;
        int o = queue[Q_KITCHEN][best];

        if (zone_reserved + orders[o].ndishes > cfg.cap_ready) {
            if (!orders[o].said_waiting) {
                orders[o].said_waiting = 1;
                log_who(WHO_COORD, 0);
                printf("заказ #%d ждёт: в зоне готовых блюд нет места (%d + %d > %d)\n",
                       o + 1, zone_reserved, orders[o].ndishes, cfg.cap_ready);
            }
            break;
        }
        q_remove(Q_KITCHEN, best);
        start_order(o);
        changed = 1;
    }
    return changed;
}

/* ===================== Повара ===================== */

/* Какое оборудование нужно текущей операции блюда id */
static int current_eq(int id)
{
    int o = ID_ORDER(id), d = ID_DISH(id);
    int m = orders[o].dish[d].menu;
    int s = orders[o].dish[d].step;
    return MENU[m].ops[s].eq;
}

/* Свободная единица оборудования нужного типа, -1 — нет */
static int free_unit(int eq)
{
    for (int u = 0; u < cfg.equipment[eq]; u++)
        if (equipment_busy[eq][u] == -1) return u;
    return -1;
}

/* Свободные повара берут операции из очереди */
int cooks_work(void)
{
    int changed = 0;
    for (int c = 0; c < cfg.cooks; c++) {
        if (cooks[c].busy) continue;   /* повар делает только одну операцию */

        /* самая приоритетная операция, для которой сейчас есть свободное оборудование */
        int best = -1;
        for (int i = 0; i < queue_len[Q_COOKS]; i++) {
            int id = queue[Q_COOKS][i];
            if (free_unit(current_eq(id)) == -1) continue;
            if (best == -1 || order_before(ID_ORDER(id), ID_ORDER(queue[Q_COOKS][best]))) best = i;
        }
        if (best == -1) break;   /* нечего делать — остальным свободным тоже */

        int id = queue[Q_COOKS][best];
        q_remove(Q_COOKS, best);
        int o = ID_ORDER(id), d = ID_DISH(id);
        int m = orders[o].dish[d].menu;
        int s = orders[o].dish[d].step;
        int eq = MENU[m].ops[s].eq;
        int u = free_unit(eq);
        equipment_busy[eq][u] = c;   /* оборудование занято этим поваром */

        cooks[c].busy = 1;
        cooks[c].order = o;
        cooks[c].dish = d;
        cooks[c].eq = eq;
        cooks[c].unit = u;
        log_who(WHO_COOK, c);
        printf("взял из очереди «%s» (%s, заказ #%d) и получил %s №%d\n",
               MENU[m].ops[s].name, MENU[m].name, o + 1, EQ_ACC[eq], u + 1);
        log_who(WHO_COOK, c);
        printf("начал «%s» — %d мин\n", MENU[m].ops[s].name, MENU[m].ops[s].time);
        add_event(now + MENU[m].ops[s].time, EV_COOK_DONE, c);
        changed = 1;
    }
    return changed;
}

/* Повар закончил операцию */
void cook_done(int c)
{
    int o = cooks[c].order, d = cooks[c].dish;
    int m = orders[o].dish[d].menu;
    int s = orders[o].dish[d].step;

    equipment_busy[cooks[c].eq][cooks[c].unit] = -1;
    cooks[c].busy = 0;
    log_who(WHO_COOK, c);
    printf("закончил «%s» (%s, заказ #%d) и освободил %s №%d\n",
           MENU[m].ops[s].name, MENU[m].name, o + 1, EQ_ACC[cooks[c].eq], cooks[c].unit + 1);

    orders[o].dish[d].step++;
    s++;
    if (s < MENU[m].nops) {
        /* следующая операция блюда начнётся только теперь, после предыдущей */
        q_push(Q_COOKS, DISH_ID(o, d));
        log_who(WHO_COORD, 0);
        printf("«%s» (%s, заказ #%d) — в очередь поваров\n", MENU[m].ops[s].name, MENU[m].name, o + 1);
        return;
    }

    /* все операции выполнены — блюдо готово */
    zone_used++;
    orders[o].ready++;
    log_who(WHO_COORD, 0);
    printf("блюдо «%s» готово → зона готовых блюд (%d/%d); сборка заказа #%d: %d из %d блюд\n",
           MENU[m].name, zone_used, cfg.cap_ready, o + 1, orders[o].ready, orders[o].ndishes);
    if (orders[o].ready == orders[o].ndishes) {
        log_who(WHO_COORD, 0);
        printf("заказ #%d собран и ждёт официанта\n", o + 1);
        q_push(Q_READY, o);
    }
}
