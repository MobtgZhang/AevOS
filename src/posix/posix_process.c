#include <posix/unistd.h>
#include "../kernel/drivers/timer.h"

pid_t posix_getpid(void)
{
    return 1;
}

unsigned int posix_sleep(unsigned int seconds)
{
    timer_sleep_ms(seconds * 1000u);
    return 0;
}
