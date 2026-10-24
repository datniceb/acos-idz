/* Меню, готовые сценарии, ввод и вывод параметров (как в ИДЗ-1).
   Ввод читается системным вызовом read(), вывод — через write() (функции из util.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "restaurant.h"

const char *EQ_NAME[EQ_TYPES] = { "плита", "печь", "гриль", "рабочий стол" };
const char *EQ_ACC[EQ_TYPES]  = { "плиту", "печь", "гриль", "рабочий стол" };

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

/* Готовые сценарии. Порядок полей как в Config:
   название, посетители, официанты, повара, {плиты, печи, грили, столы},
   блюд в заказе, интервал прихода от/до, приём, доставка,
   очередь заказов, зона готовых блюд, VIP %, приоритет, длина минуты в мс */
static const Config PRESETS[] = {
    { "Обычный вечер",                    12, 3, 4, {2, 1, 1, 2}, 3, 1, 4, 2, 1, 5, 8, 20, PRIO_FIFO, 150 },
    { "Час пик, тесная кухня",            20, 2, 3, {1, 1, 1, 1}, 3, 0, 2, 2, 1, 2, 4, 20, PRIO_VIP,  100 },
    { "Сломан гриль",                     10, 2, 3, {2, 1, 0, 2}, 2, 1, 3, 2, 1, 4, 6,  0, PRIO_FIFO, 150 },
    { "Без ограничения времени (Ctrl+C)",  0, 3, 4, {2, 1, 1, 2}, 3, 1, 4, 2, 1, 5, 8, 30, PRIO_VIP,  200 },
};
static const int NPRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

Config cfg;

/* Вывести строку в терминал (без журнала) — для вопросов */
static void put(const char *s)
{
    write_str(STDOUT_FILENO, s);
}

/* Прочитать строку с клавиатуры через read() по одному символу.
   Возвращает длину строки или -1, если ввод закончился */
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

/* Спросить целое число; Enter — значение по умолчанию */
static int ask_int(const char *prompt, int def, int lo, int hi)
{
    char line[128], text[300];
    int value;
    char extra;
    while (1) {
        snprintf(text, sizeof(text), "  %s [%d]: ", prompt, def);
        put(text);
        if (read_line(line, sizeof(line)) < 0) {   /* ввод закончился */
            put("\n");
            return def;
        }
        if (line[0] == '\0') return def;
        /* в строке должно быть ровно одно целое число и больше ничего */
        if (sscanf(line, "%d %c", &value, &extra) == 1 && value >= lo && value <= hi) return value;
        snprintf(text, sizeof(text), "    Ошибка: нужно целое число от %d до %d\n", lo, hi);
        put(text);
    }
}

static int ask_yes(const char *prompt)
{
    char line[64];
    put(prompt);
    put(" [y/N]: ");
    if (read_line(line, sizeof(line)) < 0) {
        put("\n");
        return 0;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

static void edit_config(void)
{
    put("\nВведите параметры (Enter — оставить значение по умолчанию):\n");
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

/* Выбрать сценарий и, если нужно, поменять параметры */
void read_config(void)
{
    char text[200];
    put("=== Кухня ресторана: многопоточная модель ===\n\nСценарии:\n");
    for (int i = 0; i < NPRESETS; i++) {
        snprintf(text, sizeof(text), "  %d — ", i + 1);
        put(text);
        put(PRESETS[i].title);
        put("\n");
    }
    int p = ask_int("Выберите сценарий", 1, 1, NPRESETS);
    cfg = PRESETS[p - 1];
    if (ask_yes("Изменить параметры сценария?")) {
        edit_config();
        cfg.title = "свои параметры";
    }
    if (cfg.visitors == 0 && cfg.delay_ms < 10) cfg.delay_ms = 10;   /* чтобы вывод успевал читаться */
}

/* Параметры и меню — в терминал и в журнал */
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
