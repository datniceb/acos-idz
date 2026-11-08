#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <unistd.h>

#include "shm.h"

int main(void)
{
    join_restaurant();
    signal(SIGINT, SIG_IGN);

    P(s_mutex);
    if (sh->stop_now || sh->finish || sh->waiters_here >= cfg.waiters) {
        V(s_mutex);
        write_str(STDOUT_FILENO, "Все официанты уже на месте (или ресторан закрыт) — этот официант не нужен\n");
        named_close();
        shm_detach();
        return 0;
    }
    int w = sh->waiters_here++;
    log_text("Официант %d пришёл на смену (официантов на месте: %d из %d), pid %d", w + 1, sh->waiters_here, cfg.waiters, (int)getpid());
    staff_check();
    V(s_mutex);

    waiter_role(w);

    P(s_mutex);
    log_event(WHO_WAITER, w, "ушёл со смены");
    V(s_mutex);
    V(s_gone);
    named_close();
    shm_detach();
    close_log();
    return 0;
}
