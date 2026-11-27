/*
 * Сервер — кухня ресторана. Хранит всё состояние модели: зал, очередь заказов кухни,
 * очередь поваров, оборудование, зону готовых блюд — и работает координатором: решает,
 * какой заказ брать в работу, какому повару дать операцию, какому официанту — задание.
 * Повара, официанты, посетители, вход и наблюдатели — клиенты, они подключаются по TCP.
 * Сервер однопоточный: poll() ждёт сразу и новых подключений, и сообщений от всех клиентов.
 *
 * Запуск: ./server ПОРТ
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

#define MAX_CLIENTS (MAX_VISITORS + 2 * MAX_STAFF + 30)
#define QUEUE_SIZE  (MAX_VISITORS * MAX_DISHES)

/* ===================== Клиенты ===================== */

enum { R_NEW, R_COOK, R_WAITER, R_VISITOR, R_DOOR, R_OBSERVER };

typedef struct {
    Reader in;                  /* сокет и непрочитанные байты; in.fd == -1 — место свободно */
    int role;                   /* кто это (становится известно из первой строки HELLO) */
    int id;                     /* номер повара, официанта или посетителя */
} Client;

static Client cl[MAX_CLIENTS];
static int listen_fd;

/* ===================== Состояние ресторана ===================== */

typedef struct {
    int item[QUEUE_SIZE];
    int len;
} Queue;

static Queue hall;              /* посетители, которые ждут официанта */
static Queue pass;              /* принятые заказы, которые ждут места в очереди кухни (у раздачи) */
static Queue kitchen;           /* очередь заказов кухни, не больше cap_orders */
static Queue ops;               /* очередь поваров: блюда, чья очередная операция ждёт повара (заказ*MAX_DISHES + блюдо) */
static Queue ready;             /* собранные заказы в зоне готовых блюд */

/* Посетитель v делает заказ #v */
enum { V_HALL, V_ORDERING, V_IN_WORK, V_DONE };
static Order orders[MAX_VISITORS];
static int vis_client[MAX_VISITORS];        /* клиент посетителя; -1 — отключился (ушёл) */
static int vis_state[MAX_VISITORS];
static double vis_arrive[MAX_VISITORS];

/* Повара и официанты: номер на смене → клиент (-1 — сейчас никого) */
static int cook_client[MAX_STAFF], cook_busy[MAX_STAFF], cook_order[MAX_STAFF], cook_dish[MAX_STAFF], cook_unit[MAX_STAFF];
static int cook_ops[MAX_STAFF], cook_minutes[MAX_STAFF], cook_seen[MAX_STAFF];

enum { W_FREE, W_TAKING, W_DELIVERING };
static int waiter_client[MAX_STAFF], waiter_state[MAX_STAFF], waiter_order[MAX_STAFF], waiter_hold[MAX_STAFF];
static int waiter_taken[MAX_STAFF], waiter_delivered[MAX_STAFF], waiter_seen[MAX_STAFF];

static int unit_cook[EQ_TYPES][MAX_UNITS];  /* какой повар занял единицу оборудования, -1 — свободна */
static int zone_used = 0;                   /* блюд в зоне готовых блюд */
static int zone_reserved = 0;               /* мест, забронированных под заказы в работе */
static int door_client = -1;
static int next_cook = 0, next_waiter = 0;  /* с кого начинать раздачу работы — по кругу, чтобы всем доставалось поровну */

static int arrived = 0, finished = 0;       /* сколько посетителей пришло и сколько уже ушло */
static int served = 0, cancelled = 0, left_early = 0;
static double total_time = 0;
static int dishes_done = 0, ops_done = 0, violations = 0;
static int closed = 0;                      /* вход закрыт */
static int stop_now = 0;                    /* немедленная остановка */

static volatile sig_atomic_t ctrl_c = 0;    /* сколько раз нажали Ctrl+C */

static void on_sigint(int sig)
{
    (void)sig;
    ctrl_c++;
}

/* ===================== Очереди ===================== */

static void q_push(Queue *q, int x)
{
    q->item[q->len++] = x;
}

static void q_push_front(Queue *q, int x)
{
    memmove(q->item + 1, q->item, q->len * sizeof(int));
    q->item[0] = x;
    q->len++;
}

static int q_remove(Queue *q, int i)
{
    int x = q->item[i];
    memmove(q->item + i, q->item + i + 1, (q->len - i - 1) * sizeof(int));
    q->len--;
    return x;
}

/* ===================== Вывод и отправка ===================== */

/* Отправить строку клиенту c. Если соединение уже порвано, poll это скоро покажет */
static void send_to(int c, const char *fmt, ...)
{
    char line[LINE_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (c >= 0 && cl[c].in.fd >= 0) send_line(cl[c].in.fd, "%s", line);
}

/* Строка — в терминал, в журнал и всем наблюдателям */
static void show(const char *line)
{
    log_text("%s", line);
    for (int c = 0; c < MAX_CLIENTS; c++)
        if (cl[c].in.fd >= 0 && cl[c].role == R_OBSERVER)
            send_to(c, "EV %s", line);
}

/* Событие модели: «[  12.5 мин] Повар 2: ...» */
static void event(int who, int id, const char *fmt, ...)
{
    char name[64], msg[800], line[LINE_MAX_LEN - 10];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    who_name(who, id, name, sizeof(name));
    snprintf(line, sizeof(line), "[%6.1f мин] %s: %s", model_time(), name, msg);
    show(line);
}

/* Строка без времени (итоги) */
static void text(const char *fmt, ...)
{
    char line[LINE_MAX_LEN - 10];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    show(line);
}

/* Нарушен инвариант. Если программа написана правильно, такого не бывает */
static void violation(const char *what, int id)
{
    violations++;
    event(WHO_REST, 0, "ОШИБКА: нарушен инвариант — %s (%d)", what, id + 1);
}

static int count_role(int role)
{
    int n = 0;
    for (int c = 0; c < MAX_CLIENTS; c++)
        if (cl[c].in.fd >= 0 && cl[c].role == role) n++;
    return n;
}

/* ===================== Посетители и вход ===================== */

/* Посетитель ушёл: how — обслужен, отменён или ушёл сам */
enum { F_SERVED, F_CANCELLED, F_LEFT };
static void finish_visitor(int v, int how)
{
    vis_state[v] = V_DONE;
    finished++;
    if (how == F_SERVED) served++;
    else if (how == F_CANCELLED) cancelled++;
    else left_early++;
}

static void close_intake(const char *why)
{
    if (closed) return;
    closed = 1;
    event(WHO_REST, 0, "%s", why);
    if (door_client >= 0) send_to(door_client, "CLOSE");
}

/* Посетитель пришёл со своим заказом */
static void new_visitor(int c, const char *order_text)
{
    Order o;
    if (closed || (cfg.visitors > 0 && arrived >= cfg.visitors) || arrived == MAX_VISITORS) {
        send_to(c, "BYE вход закрыт");
        close(cl[c].in.fd);
        cl[c].in.fd = -1;
        return;
    }
    if (order_from_text(order_text, &o) < 0) {
        send_to(c, "BYE непонятный заказ");
        close(cl[c].in.fd);
        cl[c].in.fd = -1;
        return;
    }
    int v = arrived++;
    orders[v] = o;
    vis_client[v] = c;
    vis_state[v] = V_HALL;
    vis_arrive[v] = model_time();
    cl[c].role = R_VISITOR;
    cl[c].id = v;
    send_to(c, "WELCOME %d %.3f %d", v, model_time(), cfg.delay_ms);
    q_push(&hall, v);
    event(WHO_VISITOR, v, "пришёл в ресторан%s и ждёт официанта (ждут в зале: %d)", o.vip ? " (VIP)" : "", hall.len);
    if (cfg.visitors > 0 && arrived == cfg.visitors) {
        char why[100];
        snprintf(why, sizeof(why), "вход закрыт: пришли все %d посетителей", cfg.visitors);
        close_intake(why);
    }
}

/* ===================== Повара ===================== */

/* Есть ли свободная единица оборудования типа eq; номер или -1 */
static int free_unit(int eq)
{
    for (int u = 0; u < cfg.equipment[eq]; u++)
        if (unit_cook[eq][u] < 0) return u;
    return -1;
}

/* Повар c закончил свою операцию */
static void cook_done(int c)
{
    if (!cook_busy[c]) return;
    int o = cook_order[c], d = cook_dish[c], u = cook_unit[c];
    Dish *dish = &orders[o].dish[d];
    const MenuDish *md = &MENU[dish->menu];
    Operation op = md->ops[dish->step];

    if (unit_cook[op.eq][u] != c) violation("оборудование во время готовки занял другой повар", c);
    unit_cook[op.eq][u] = -1;
    cook_busy[c] = 0;
    cook_ops[c]++;
    cook_minutes[c] += op.time;
    ops_done++;
    event(WHO_COOK, c, "закончил «%s» (заказ #%d, блюдо %d: %s) и освободил %s №%d",
          op.name, o + 1, d + 1, md->name, EQ_ACC[op.eq], u + 1);

    dish->step++;
    if (dish->step < md->nops) {
        /* следующая операция блюда попадает в очередь только теперь, после предыдущей */
        q_push(&ops, o * MAX_DISHES + d);
        event(WHO_COORD, 0, "«%s» (заказ #%d, блюдо %d: %s) — в очередь поваров", md->ops[dish->step].name, o + 1, d + 1, md->name);
        return;
    }
    zone_used++;
    orders[o].ready++;
    dishes_done++;
    event(WHO_COORD, 0, "блюдо %d заказа #%d «%s» готово → зона готовых блюд (%d/%d); сборка заказа #%d: %d из %d блюд",
          d + 1, o + 1, md->name, zone_used, cfg.cap_ready, o + 1, orders[o].ready, orders[o].ndishes);
    if (zone_used > cfg.cap_ready) violation("в зоне готовых блюд больше блюд, чем мест", o);
    if (orders[o].ready == orders[o].ndishes) {
        event(WHO_COORD, 0, "заказ #%d собран и ждёт официанта", o + 1);
        q_push(&ready, o);
    }
}

/* Дать свободным поварам операции: самую приоритетную среди тех, для которых
   есть свободное оборудование */
static void give_cooks_work(void)
{
    int first = next_cook;
    for (int k = 0; k < MAX_STAFF; k++) {
        int c = (first + k) % MAX_STAFF;
        if (cook_client[c] < 0 || cook_busy[c]) continue;
        int best = -1;
        for (int i = 0; i < ops.len; i++) {
            int o = ops.item[i] / MAX_DISHES, d = ops.item[i] % MAX_DISHES;
            const Dish *dish = &orders[o].dish[d];
            if (free_unit(MENU[dish->menu].ops[dish->step].eq) < 0) continue;
            if (best < 0) {
                best = i;
            } else {
                int b = ops.item[best] / MAX_DISHES;
                if (order_before(o, orders[o].vip, b, orders[b].vip)) best = i;
            }
        }
        if (best < 0) return;           /* для операций в очереди всё нужное оборудование занято */

        int x = q_remove(&ops, best);
        int o = x / MAX_DISHES, d = x % MAX_DISHES;
        const Dish *dish = &orders[o].dish[d];
        const MenuDish *md = &MENU[dish->menu];
        Operation op = md->ops[dish->step];
        int u = free_unit(op.eq);
        unit_cook[op.eq][u] = c;
        cook_busy[c] = 1;
        cook_order[c] = o;
        cook_dish[c] = d;
        cook_unit[c] = u;
        next_cook = (c + 1) % MAX_STAFF;
        send_to(cook_client[c], "COOK %d %d %d %d %d", o, d, dish->menu, dish->step, u);
        event(WHO_COOK, c, "получил %s №%d и начал «%s» (заказ #%d, блюдо %d: %s), шаг %d/%d — %d мин",
              EQ_ACC[op.eq], u + 1, op.name, o + 1, d + 1, md->name, dish->step + 1, md->nops, op.time);
    }
}

/* ===================== Координатор ===================== */

/* Взять заказ в работу: забронировать места в зоне и поставить первые операции блюд в очередь поваров */
static void start_order(int o)
{
    Order *ord = &orders[o];
    zone_reserved += ord->ndishes;
    event(WHO_COORD, 0, "заказ #%d%s взят в работу, в зоне готовых блюд забронировано %d/%d мест",
          o + 1, ord->vip ? " (VIP)" : "", zone_reserved, cfg.cap_ready);
    for (int i = 0; i < ord->ndishes; i++) {
        const MenuDish *md = &MENU[ord->dish[i].menu];
        for (int j = 0; j < md->nops; j++)
            event(WHO_COORD, 0, "  создана операция: блюдо %d «%s», шаг %d — %s (%s, %d мин)",
                  i + 1, md->name, j + 1, md->ops[j].name, EQ_NAME[md->ops[j].eq], md->ops[j].time);
        event(WHO_COORD, 0, "  «%s» (заказ #%d, блюдо %d: %s) — в очередь поваров", md->ops[0].name, o + 1, i + 1, md->name);
        q_push(&ops, o * MAX_DISHES + i);
    }
}

/* Какой официант держит заказ o у раздачи (-1 — никто: официант отключился) */
static int holder(int o)
{
    for (int w = 0; w < MAX_STAFF; w++)
        if (waiter_client[w] >= 0 && waiter_hold[w] == o) return w;
    return -1;
}

/* Координатор: передвинуть заказы из-под раздачи в очередь кухни и взять заказы в работу.
   Возвращает 1, если что-то поменялось */
static int coordinator_step(void)
{
    int changed = 0;
    while (pass.len > 0 && kitchen.len < cfg.cap_orders) {
        int o = q_remove(&pass, 0);
        q_push(&kitchen, o);
        int w = holder(o);
        if (w >= 0) {
            waiter_hold[w] = -1;
            send_to(waiter_client[w], "PASSED %d", o);
            event(WHO_WAITER, w, "передал заказ #%d на кухню (очередь заказов %d/%d)", o + 1, kitchen.len, cfg.cap_orders);
        } else {
            event(WHO_COORD, 0, "заказ #%d, оставленный у раздачи, принят на кухню (очередь заказов %d/%d)", o + 1, kitchen.len, cfg.cap_orders);
        }
        changed = 1;
    }
    while (kitchen.len > 0) {
        int best = 0;
        for (int i = 1; i < kitchen.len; i++) {
            int a = kitchen.item[i], b = kitchen.item[best];
            if (order_before(a, orders[a].vip, b, orders[b].vip)) best = i;
        }
        int o = kitchen.item[best];
        const char *why = check_order(&orders[o]);
        if (why != NULL) {                  /* невыполнимый заказ отменяем */
            q_remove(&kitchen, best);
            event(WHO_COORD, 0, "заказ #%d невыполним: %s. Заказ отменён", o + 1, why);
            if (vis_client[o] >= 0) {
                send_to(vis_client[o], "CANCELLED %s", why);
                event(WHO_VISITOR, o, "ушёл без заказа");
            }
            finish_visitor(o, F_CANCELLED);
            changed = 1;
            continue;
        }
        /* места бронируются сразу под все блюда заказа: иначе зона может забиться
           блюдами разных недоделанных заказов, и ни один заказ не соберётся */
        if (zone_reserved + orders[o].ndishes > cfg.cap_ready) {
            if (!orders[o].said_waiting) {
                orders[o].said_waiting = 1;
                event(WHO_COORD, 0, "заказ #%d ждёт: в зоне готовых блюд нет места (%d + %d > %d)",
                      o + 1, zone_reserved, orders[o].ndishes, cfg.cap_ready);
            }
            break;
        }
        q_remove(&kitchen, best);
        start_order(o);
        changed = 1;
    }
    return changed;
}

/* ===================== Официанты ===================== */

/* Официант w закончил принимать заказ */
static void waiter_took(int w)
{
    if (waiter_state[w] != W_TAKING) return;
    int v = waiter_order[w];
    waiter_state[w] = W_FREE;
    if (vis_client[v] < 0) {
        event(WHO_WAITER, w, "принял заказ, но посетитель %d уже ушёл", v + 1);
        finish_visitor(v, F_LEFT);
        return;
    }
    char list[400];
    dishes_text(&orders[v], list, sizeof(list));
    waiter_taken[w]++;
    vis_state[v] = V_IN_WORK;
    event(WHO_WAITER, w, "принял заказ #%d у посетителя %d%s: %s", v + 1, v + 1, orders[v].vip ? " (VIP)" : "", list);
    if (kitchen.len < cfg.cap_orders && pass.len == 0) {
        q_push(&kitchen, v);
        send_to(waiter_client[w], "PASSED %d", v);
        event(WHO_WAITER, w, "передал заказ #%d на кухню (очередь заказов %d/%d)", v + 1, kitchen.len, cfg.cap_orders);
    } else {
        /* очередь кухни заполнена: официант ждёт у раздачи и пока может разносить
           собранные заказы, иначе все официанты встанут у раздачи и ресторан остановится */
        q_push(&pass, v);
        waiter_hold[w] = v;
        send_to(waiter_client[w], "HOLD %d", v);
        event(WHO_WAITER, w, "очередь заказов кухни заполнена (%d/%d), ждёт у раздачи с заказом #%d",
              kitchen.len, cfg.cap_orders, v + 1);
    }
}

/* Официант w донёс заказ */
static void waiter_gave(int w)
{
    if (waiter_state[w] != W_DELIVERING) return;
    int o = waiter_order[w];
    waiter_state[w] = W_FREE;
    zone_used -= orders[o].ndishes;
    zone_reserved -= orders[o].ndishes;
    if (orders[o].ready != orders[o].ndishes) violation("заказ выдан не готовым", o);
    if (vis_client[o] < 0) {
        event(WHO_WAITER, w, "принёс заказ #%d, но посетитель %d уже ушёл", o + 1, o + 1);
        finish_visitor(o, F_LEFT);
        return;
    }
    double t = model_time() - vis_arrive[o];
    waiter_delivered[w]++;
    total_time += t;
    event(WHO_WAITER, w, "выдал заказ #%d посетителю %d", o + 1, o + 1);
    send_to(vis_client[o], "SERVED %d %.1f", w, t);
    event(WHO_VISITOR, o, "получил свой заказ #%d и ушёл, провёл в ресторане %.1f мин", o + 1, t);
    finish_visitor(o, F_SERVED);
}

/* Дать свободным официантам задания: сначала разносить собранные заказы,
   потом подходить к посетителям в зале (если официант не стоит у раздачи с заказом) */
static void give_waiters_work(void)
{
    int first = next_waiter;
    for (int k = 0; k < MAX_STAFF; k++) {
        int w = (first + k) % MAX_STAFF;
        if (waiter_client[w] < 0 || waiter_state[w] != W_FREE) continue;
        if (ready.len > 0) {
            next_waiter = (w + 1) % MAX_STAFF;
            int o = q_remove(&ready, 0);
            waiter_state[w] = W_DELIVERING;
            waiter_order[w] = o;
            send_to(waiter_client[w], "DELIVER %d %d", o, cfg.deliver_time);
            event(WHO_WAITER, w, "забрал заказ #%d из зоны готовых блюд и несёт посетителю %d", o + 1, o + 1);
        } else if (waiter_hold[w] < 0 && hall.len > 0) {
            next_waiter = (w + 1) % MAX_STAFF;
            int v = q_remove(&hall, 0);
            char order_text[100];
            order_to_text(&orders[v], order_text, sizeof(order_text));
            waiter_state[w] = W_TAKING;
            waiter_order[w] = v;
            vis_state[v] = V_ORDERING;
            send_to(waiter_client[w], "TAKE %d %d %s", v, cfg.take_time, order_text);
            send_to(vis_client[v], "WAITER %d", w);
            event(WHO_WAITER, w, "подошёл к посетителю %d и принимает заказ", v + 1);
        }
    }
}

/* ===================== Подключение и отключение клиентов ===================== */

static int free_slot(int *client_of)
{
    for (int i = 0; i < MAX_STAFF; i++)
        if (client_of[i] < 0) return i;
    return -1;
}

/* Первая строка от клиента: кто он */
static void hello(int c, const char *line)
{
    if (strcmp(line, "HELLO COOK") == 0) {
        int id = free_slot(cook_client);
        if (id < 0) {
            send_to(c, "BYE на кухне уже %d поваров", MAX_STAFF);
            close(cl[c].in.fd);
            cl[c].in.fd = -1;
            return;
        }
        cook_client[id] = c;
        cook_busy[id] = 0;
        cook_seen[id] = 1;
        cl[c].role = R_COOK;
        cl[c].id = id;
        send_to(c, "WELCOME %d %.3f %d", id, model_time(), cfg.delay_ms);
        event(WHO_COOK, id, "пришёл на смену (поваров на кухне: %d)", count_role(R_COOK));
    } else if (strcmp(line, "HELLO WAITER") == 0) {
        int id = free_slot(waiter_client);
        if (id < 0) {
            send_to(c, "BYE в зале уже %d официантов", MAX_STAFF);
            close(cl[c].in.fd);
            cl[c].in.fd = -1;
            return;
        }
        waiter_client[id] = c;
        waiter_state[id] = W_FREE;
        waiter_hold[id] = -1;
        waiter_seen[id] = 1;
        cl[c].role = R_WAITER;
        cl[c].id = id;
        send_to(c, "WELCOME %d %.3f %d", id, model_time(), cfg.delay_ms);
        event(WHO_WAITER, id, "пришёл на смену (официантов в зале: %d)", count_role(R_WAITER));
    } else if (strncmp(line, "HELLO VISITOR ", 14) == 0) {
        new_visitor(c, line + 14);
    } else if (strcmp(line, "HELLO DOOR") == 0) {
        if (door_client >= 0) {
            send_to(c, "BYE вход уже работает");
            close(cl[c].in.fd);
            cl[c].in.fd = -1;
            return;
        }
        door_client = c;
        cl[c].role = R_DOOR;
        send_to(c, "WELCOME 0 %.3f %d", model_time(), cfg.delay_ms);
        /* параметры для входа: сколько всего посетителей, сколько уже пришло, интервалы, заказы */
        send_to(c, "PARAMS %d %d %d %d %d %d", cfg.visitors, arrived, cfg.arrive_min, cfg.arrive_max,
                cfg.max_dishes, cfg.vip_percent);
        if (closed) send_to(c, "CLOSE");
        event(WHO_DOOR, 0, arrived == 0 ? "открыт, впускает посетителей" : "подключён снова — посетители приходят дальше");
    } else if (strcmp(line, "HELLO OBSERVER") == 0) {
        cl[c].role = R_OBSERVER;
        send_to(c, "WELCOME 0 %.3f %d", model_time(), cfg.delay_ms);
        send_to(c, "EV [%6.1f мин] Ресторан: сейчас пришло посетителей %d, ушло %d; в зале ждут %d; "
                   "очередь заказов кухни %d/%d, у раздачи %d; в зоне готовых блюд %d/%d; поваров %d, официантов %d",
                model_time(), arrived, finished, hall.len, kitchen.len, cfg.cap_orders, pass.len,
                zone_used, cfg.cap_ready, count_role(R_COOK), count_role(R_WAITER));
        event(WHO_REST, 0, "подключился наблюдатель (наблюдателей: %d)", count_role(R_OBSERVER));
    } else {
        send_to(c, "BYE непонятное приветствие");
        close(cl[c].in.fd);
        cl[c].in.fd = -1;
    }
}

/* Клиент отключился (сам, по Ctrl+C или kill). Его незаконченная работа возвращается,
   и ресторан работает дальше: клиента можно запустить снова */
static void drop_client(int c)
{
    int id = cl[c].id, role = cl[c].role;
    close(cl[c].in.fd);
    cl[c].in.fd = -1;

    if (role == R_COOK) {
        cook_client[id] = -1;
        if (cook_busy[id]) {
            int o = cook_order[id], d = cook_dish[id];
            const Dish *dish = &orders[o].dish[d];
            Operation op = MENU[dish->menu].ops[dish->step];
            unit_cook[op.eq][cook_unit[id]] = -1;
            cook_busy[id] = 0;
            q_push_front(&ops, o * MAX_DISHES + d);
            event(WHO_COOK, id, "отключился посреди операции «%s» (заказ #%d) — оборудование (%s №%d) освободилось, операция вернулась в очередь поваров",
                  op.name, o + 1, EQ_NAME[op.eq], cook_unit[id] + 1);
        } else {
            event(WHO_COOK, id, "ушёл со смены (отключился)");
        }
    } else if (role == R_WAITER) {
        waiter_client[id] = -1;
        if (waiter_state[id] == W_TAKING) {
            int v = waiter_order[id];
            if (vis_client[v] >= 0) {
                q_push_front(&hall, v);
                vis_state[v] = V_HALL;
                send_to(vis_client[v], "WAITER_LEFT");
                event(WHO_WAITER, id, "отключился, не приняв заказ — посетитель %d снова ждёт официанта", v + 1);
            } else {
                finish_visitor(v, F_LEFT);
            }
        } else if (waiter_state[id] == W_DELIVERING) {
            q_push_front(&ready, waiter_order[id]);
            event(WHO_WAITER, id, "отключился по дороге — заказ #%d вернулся в зону готовых блюд", waiter_order[id] + 1);
        } else if (waiter_hold[id] >= 0) {
            event(WHO_WAITER, id, "отключился — заказ #%d остался у раздачи, кухня примет его сама", waiter_hold[id] + 1);
        } else {
            event(WHO_WAITER, id, "ушёл со смены (отключился)");
        }
        waiter_state[id] = W_FREE;
        waiter_hold[id] = -1;
    } else if (role == R_VISITOR) {
        vis_client[id] = -1;
        if (vis_state[id] == V_HALL) {
            for (int i = 0; i < hall.len; i++)
                if (hall.item[i] == id) q_remove(&hall, i);
            event(WHO_VISITOR, id, "ушёл, не дождавшись официанта");
            finish_visitor(id, F_LEFT);
        } else if (vis_state[id] != V_DONE) {
            event(WHO_VISITOR, id, "ушёл, не дождавшись заказа (заказ всё равно доготовят)");
        }
    } else if (role == R_DOOR) {
        door_client = -1;
        if (!closed)
            event(WHO_DOOR, 0, "отключился — новые посетители не приходят, пока вход не запустят снова");
        else
            event(WHO_DOOR, 0, "отключился (вход уже закрыт)");
    } else if (role == R_OBSERVER) {
        event(WHO_REST, 0, "наблюдатель отключился (наблюдателей: %d)", count_role(R_OBSERVER));
    }
}

static void accept_client(void)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;
    for (int c = 0; c < MAX_CLIENTS; c++) {
        if (cl[c].in.fd < 0) {
            cl[c].in.fd = fd;
            cl[c].in.len = 0;
            cl[c].role = R_NEW;
            cl[c].id = 0;
            return;
        }
    }
    close(fd);                              /* мест нет */
}

/* Одна строка от клиента c */
static void handle_line(int c, const char *line)
{
    int id = cl[c].id;
    if (cl[c].role == R_NEW)
        hello(c, line);
    else if (cl[c].role == R_COOK && strcmp(line, "DONE") == 0)
        cook_done(id);
    else if (cl[c].role == R_WAITER && strcmp(line, "TAKEN") == 0)
        waiter_took(id);
    else if (cl[c].role == R_WAITER && strcmp(line, "DELIVERED") == 0)
        waiter_gave(id);
}

/* Прочитать всё, что прислал клиент, и разобрать по строкам */
static void read_client(int c)
{
    Reader *r = &cl[c].in;
    int n = read(r->fd, r->buf + r->len, sizeof(r->buf) - r->len);
    if (n <= 0) {
        if (n < 0 && errno == EINTR) return;
        drop_client(c);
        return;
    }
    r->len += n;
    char line[LINE_MAX_LEN];
    while (cl[c].in.fd >= 0 && take_line(r, line, sizeof(line)))
        handle_line(c, line);
    if (cl[c].in.fd >= 0 && r->len == (int)sizeof(r->buf))
        drop_client(c);                     /* строка без конца — клиент сломан */
}

/* ===================== Итоги ===================== */

static void print_summary(void)
{
    text("\n=== Итоги ===");
    text("Модельное время: %.1f мин", model_time());
    text("Пришло посетителей: %d", arrived);
    if (served > 0)
        text("Обслужено: %d (в среднем %.1f мин в ресторане)", served, total_time / served);
    else
        text("Обслужено: 0");
    text("Отменено невыполнимых заказов: %d", cancelled);
    text("Ушли сами (отключились): %d", left_early);
    text("Не обслужено к моменту остановки: %d", arrived - finished);
    text("Приготовлено блюд: %d, выполнено операций: %d", dishes_done, ops_done);
    for (int w = 0; w < MAX_STAFF; w++)
        if (waiter_seen[w])
            text("  Официант %d: принял заказов — %d, выдал — %d", w + 1, waiter_taken[w], waiter_delivered[w]);
    for (int c = 0; c < MAX_STAFF; c++)
        if (cook_seen[c])
            text("  Повар %d: операций — %d, готовил %d мин", c + 1, cook_ops[c], cook_minutes[c]);
    text("Нарушений инвариантов: %d", violations);
}

/* ===================== main ===================== */

int main(int argc, char **argv)
{
    if (argc != 2 || parse_port(argv[1]) < 0) {
        write_str(STDERR_FILENO, "Запуск: ./server ПОРТ   (например ./server 5000)\n");
        return 1;
    }
    int port = parse_port(argv[1]);
    signal(SIGPIPE, SIG_IGN);       /* клиент может отключиться, пока ему пишут, — это не ошибка */

    /* порт открываем сразу: клиенты, запущенные раньше, подключатся и подождут, пока введут параметры */
    listen_fd = listen_on(port);
    if (listen_fd < 0) {
        write_str(STDERR_FILENO, "Ошибка: не удалось открыть порт (может быть, он уже занят)\n");
        return 1;
    }
    for (int c = 0; c < MAX_CLIENTS; c++) cl[c].in.fd = -1;
    for (int i = 0; i < MAX_STAFF; i++) cook_client[i] = waiter_client[i] = waiter_hold[i] = -1;
    for (int t = 0; t < EQ_TYPES; t++)
        for (int u = 0; u < MAX_UNITS; u++) unit_cook[t][u] = -1;

    read_config();
    open_log("restaurant_server.log");
    print_config();

    struct sigaction sa;            /* Ctrl+C. Без SA_RESTART: сигнал прерывает poll */
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    log_text("Сервер слушает порт %d. Клиенты запускаются так (IP — адрес этого компьютера):", port);
    log_text("  ./cook IP %d   ./waiter IP %d   ./door IP %d   ./observer IP %d", port, port, port, port);
    log_text("Ctrl+C здесь: если вход открыт — закрыть его и дообслужить тех, кто внутри; если закрыт — остановить сразу\n");
    start_clock(0);
    event(WHO_REST, 0, "ресторан открыт, ждём сотрудников и посетителей");

    static struct pollfd pfd[MAX_CLIENTS + 1];
    static int who[MAX_CLIENTS + 1];        /* pfd[i] — сокет клиента who[i] */
    int used = 0;
    while (1) {
        /* Ctrl+C: если вход открыт — закрыть его, если закрыт — остановить сразу */
        while (ctrl_c > used) {
            used++;
            if (!closed) {
                close_intake("получен Ctrl+C — вход закрыт, дообслуживаем тех, кто внутри");
            } else {
                stop_now = 1;
                event(WHO_REST, 0, "получен Ctrl+C — немедленная остановка");
            }
        }
        if (stop_now || (closed && finished == arrived)) break;

        int n = 0;
        pfd[n].fd = listen_fd;
        pfd[n].events = POLLIN;
        who[n++] = -1;
        for (int c = 0; c < MAX_CLIENTS; c++) {
            if (cl[c].in.fd >= 0) {
                pfd[n].fd = cl[c].in.fd;
                pfd[n].events = POLLIN;
                who[n++] = c;
            }
        }
        if (poll(pfd, n, -1) < 0) {
            if (errno == EINTR) continue;   /* пришёл Ctrl+C */
            break;
        }
        if (pfd[0].revents & POLLIN) accept_client();
        for (int i = 1; i < n; i++) {
            int c = who[i];
            /* клиента могли уже закрыть, пока разбирали других */
            if (pfd[i].revents && cl[c].in.fd == pfd[i].fd) read_client(c);
        }
        /* раздаём работу, пока что-то меняется */
        while (coordinator_step()) {
        }
        give_cooks_work();
        give_waiters_work();
    }

    if (stop_now)
        event(WHO_REST, 0, "моделирование остановлено");
    else
        event(WHO_REST, 0, "все посетители ушли — ресторан закрывается");
    print_summary();
    log_text("Журнал сервера сохранён в файл restaurant_server.log");

    /* при завершении сервера все клиенты получают BYE и тоже завершаются */
    for (int c = 0; c < MAX_CLIENTS; c++) {
        if (cl[c].in.fd >= 0) {
            send_to(c, stop_now ? "BYE ресторан закрыт немедленно" : "BYE ресторан закрылся");
            close(cl[c].in.fd);
        }
    }
    close(listen_fd);
    close_log();
    return 0;
}
