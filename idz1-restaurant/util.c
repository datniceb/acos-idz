/* Вывод событий, случайные числа, задержка, очереди */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "restaurant.h"

/* Начало строки события: время и кто. Остальное печатает вызывающий через printf */
void log_who(int who, int id)
{
    printf("[%4ld мин] ", now);
    if (who == WHO_VISITOR)     printf("Посетитель %d: ", id + 1);
    else if (who == WHO_WAITER) printf("Официант %d: ", id + 1);
    else if (who == WHO_COOK)   printf("Повар %d: ", id + 1);
    else if (who == WHO_COORD)  printf("Координатор: ");
    else                        printf("Ресторан: ");
}

/* Напечатать блюда заказа через запятую */
void print_dishes(int o)
{
    for (int i = 0; i < orders[o].ndishes; i++) {
        if (i > 0) printf(", ");
        printf("%s", MENU[orders[o].dish[i].menu].name);
    }
}

/* Случайное целое от lo до hi включительно */
int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

void sleep_ms(long ms)
{
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);   /* Ctrl+C прерывает паузу */
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
