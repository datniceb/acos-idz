#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <unistd.h>

#include "shm.h"

int main(void)
{
    join_restaurant();
    signal(SIGINT, SIG_IGN);

    P(s_mutex);
    if (sh->stop_now || sh->finish || sh->cooks_here >= cfg.cooks) {
        V(s_mutex);
        write_str(STDOUT_FILENO, "Все повара уже на месте (или ресторан закрыт) — этот повар не нужен\n");
        named_close();
        shm_detach();
        return 0;
    }
    int c = sh->cooks_here++;
    log_text("Повар %d пришёл на смену (поваров на месте: %d из %d), pid %d", c + 1, sh->cooks_here, cfg.cooks, (int)getpid());
    staff_check();
    V(s_mutex);

    cook_role(c);

    P(s_mutex);
    log_event(WHO_COOK, c, "ушёл со смены");
    V(s_mutex);
    V(s_gone);
    named_close();
    shm_detach();
    close_log();
    return 0;
}
