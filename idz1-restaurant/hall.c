/* Зал ресторана: посетители и официанты */
#include <stdio.h>
#include <stdlib.h>

#include "restaurant.h"

/* ===================== Посетители ===================== */

/* Пришёл новый посетитель. Он сразу решает, что закажет */
void visitor_arrives(void)
{
    int v = n_visitors;
    n_visitors++;
    visitors[v].state = V_WAIT_WAITER;
    visitors[v].arrive_time = now;

    orders[v].vip = rand() % 100 < cfg.vip_percent;
    orders[v].ndishes = rand_range(1, cfg.max_dishes);
    orders[v].ready = 0;
    orders[v].said_waiting = 0;
    for (int i = 0; i < orders[v].ndishes; i++) {
        orders[v].dish[i].menu = rand() % MENU_SIZE;
        orders[v].dish[i].step = 0;
    }
    log_who(WHO_VISITOR, v);
    printf("пришёл в ресторан%s и ждёт официанта (ждут в зале: %d)\n",
           orders[v].vip ? " (VIP)" : "", queue_len[Q_HALL] + 1);
    q_push(Q_HALL, v);

    /* когда придёт следующий */
    if (cfg.visitors > 0 && n_visitors == cfg.visitors) {
        intake_closed = 1;
        log_who(WHO_REST, 0);
        printf("вход закрыт: пришли все %d посетителей\n", cfg.visitors);
    } else if (n_visitors == MAX_VISITORS) {
        intake_closed = 1;
        log_who(WHO_REST, 0);
        printf("вход закрыт: в модели не может быть больше %d посетителей\n", MAX_VISITORS);
    } else {
        add_event(now + rand_range(cfg.arrive_min, cfg.arrive_max), EV_ARRIVAL, 0);
    }
}

/* ===================== Официанты ===================== */

static int find_waiter(int state)
{
    for (int w = 0; w < cfg.waiters; w++)
        if (waiters[w].state == state) return w;
    return -1;
}

/* Отдать заказ на кухню. Если очередь кухни заполнена — ждать у раздачи */
static void give_to_kitchen(int w)
{
    int o = waiters[w].slip;
    if (queue_len[Q_KITCHEN] >= cfg.cap_orders) {
        waiters[w].state = W_AT_KITCHEN;
        log_who(WHO_WAITER, w);
        printf("очередь заказов кухни заполнена (%d/%d), ждёт у раздачи с заказом #%d\n",
               queue_len[Q_KITCHEN], cfg.cap_orders, o + 1);
        return;
    }
    q_push(Q_KITCHEN, o);
    waiters[w].slip = -1;
    waiters[w].state = W_FREE;
    log_who(WHO_WAITER, w);
    printf("передал заказ #%d на кухню (очередь заказов %d/%d)\n",
           o + 1, queue_len[Q_KITCHEN], cfg.cap_orders);
}

/* Свободные официанты берут работу. Возвращает 1, если что-то изменилось */
int waiters_work(void)
{
    int changed = 0;

    /* 1. Сначала разносим собранные заказы. Их может взять и официант,
          который ждёт у раздачи, иначе зона готовых блюд не освободится */
    while (queue_len[Q_READY] > 0) {
        int w = find_waiter(W_FREE);
        if (w < 0) w = find_waiter(W_AT_KITCHEN);
        if (w < 0) break;
        int o = queue[Q_READY][0];
        q_remove(Q_READY, 0);
        waiters[w].state = W_DELIVERING;
        waiters[w].carrying = o;
        zone_used -= orders[o].ndishes;
        zone_reserved -= orders[o].ndishes;
        log_who(WHO_WAITER, w);
        printf("забрал заказ #%d из зоны готовых блюд (%d/%d) и несёт посетителю %d\n",
               o + 1, zone_used, cfg.cap_ready, o + 1);
        add_event(now + cfg.deliver_time, EV_DELIVERED, w);
        changed = 1;
    }

    /* 2. Официанты у раздачи отдают заказ, если в очереди кухни появилось место */
    for (int w = 0; w < cfg.waiters; w++) {
        if (waiters[w].state == W_AT_KITCHEN && queue_len[Q_KITCHEN] < cfg.cap_orders) {
            give_to_kitchen(w);
            changed = 1;
        }
    }

    /* 3. Свободные официанты подходят к посетителям в порядке прихода */
    while (queue_len[Q_HALL] > 0) {
        int w = find_waiter(W_FREE);
        if (w < 0) break;
        int v = queue[Q_HALL][0];
        q_remove(Q_HALL, 0);
        waiters[w].state = W_TAKING;
        waiters[w].visitor = v;
        visitors[v].state = V_ORDERING;
        log_who(WHO_WAITER, w);
        printf("подошёл к посетителю %d и принимает заказ\n", v + 1);
        add_event(now + cfg.take_time, EV_ORDER_TAKEN, w);
        changed = 1;
    }
    return changed;
}

/* Официант принял заказ */
void order_taken(int w)
{
    int v = waiters[w].visitor;
    visitors[v].state = V_WAIT_FOOD;
    log_who(WHO_WAITER, w);
    printf("принял заказ #%d у посетителя %d: ", v + 1, v + 1);
    print_dishes(v);
    printf("\n");

    /* координатор сразу проверяет, можно ли приготовить заказ */
    if (!check_order(v)) {
        log_who(WHO_VISITOR, v);
        printf("ушёл без заказа\n");
        visitors[v].state = V_LEFT;
        n_cancelled++;
        waiters[w].state = W_FREE;
        return;
    }
    waiters[w].slip = v;
    give_to_kitchen(w);
}

/* Официант донёс заказ */
void order_delivered(int w)
{
    int o = waiters[w].carrying;
    int v = o;   /* номер заказа совпадает с номером посетителя */

    /* инвариант: заказ отдаём только своему посетителю, который его ждёт,
       и только когда готовы все блюда */
    if (visitors[v].state != V_WAIT_FOOD || orders[o].ready != orders[o].ndishes) {
        printf("ОШИБКА: нарушен инвариант выдачи заказа #%d\n", o + 1);
        exit(1);
    }

    long wait = now - visitors[v].arrive_time;
    total_wait += wait;
    n_served++;
    visitors[v].state = V_SERVED;
    log_who(WHO_WAITER, w);
    printf("выдал заказ #%d посетителю %d\n", o + 1, v + 1);
    log_who(WHO_VISITOR, v);
    printf("получил свой заказ и ушёл, провёл в ресторане %ld мин\n", wait);

    waiters[w].carrying = -1;
    if (waiters[w].slip >= 0)
        waiters[w].state = W_AT_KITCHEN;   /* вернулся к раздаче со своим заказом */
    else
        waiters[w].state = W_FREE;
}
