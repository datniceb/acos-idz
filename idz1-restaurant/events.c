/* Список запланированных событий */
#include "restaurant.h"

/* Одновременно ждут не больше одного прихода посетителя
   и по одному событию на каждого официанта и повара */
#define MAX_EVENTS (1 + 2 * MAX_STAFF)

static Event events[MAX_EVENTS];
static int n_events;
static long counter;

void add_event(long time, int type, int who)
{
    events[n_events].time = time;
    events[n_events].num = counter;
    events[n_events].type = type;
    events[n_events].who = who;
    n_events++;
    counter++;
}

int no_events(void)
{
    return n_events == 0;
}

/* Достать самое раннее событие (при равном времени — запланированное раньше) */
Event next_event(void)
{
    int best = 0;
    for (int i = 1; i < n_events; i++) {
        if (events[i].time < events[best].time ||
            (events[i].time == events[best].time && events[i].num < events[best].num))
            best = i;
    }
    Event e = events[best];
    events[best] = events[n_events - 1];   /* на освободившееся место — последнее */
    n_events--;
    return e;
}
