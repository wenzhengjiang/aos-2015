#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <clock/clock.h>
#include <cspace/cspace.h>
#include <sel4/types.h>
#include <assert.h>
#include <string.h>
#include <sync/mutex.h>

#define MAX_CALLBACK_ID 20

static const timestamp_t MAX_TICK = (1ULL<<63) - 1;

struct callback {
    bool valid;
    uint32_t id;
    uint64_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
};

typedef struct callback callback_t;

static callback_t callback_arr[MAX_CALLBACK_ID+1];
static sync_mutex_t callback_m;

static bool initialized;

static timestamp_t last_tick;

int start_timer(seL4_CPtr interrupt_ep)
{
    (void)interrupt_ep;
    callback_m = sync_create_mutex();
    initialized = true;
    return 0;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback_fun, void *data) {
    if (callback_fun == NULL) {
        dprintf(0, "invalid callback_fun\n");
        return 0;
    }
    if (!initialized) {
        dprintf(0, "timer hasn't been initialised \n");
        return 0;
    }
    timestamp_t curtick = time_stamp();
    callback_t *cb = NULL;
    for (int i = 1;i <= MAX_CALLBACK_ID; i++) {
        if (!callback_arr[i].valid) {
            cb = &callback_arr[i];
            cb->valid = true;
            cb->id = i;
            cb->delay = delay;
            if (MAX_TICK - curtick < delay) {
                cb->next_timeout = MAX_TICK; // workaround for overflow
            } else {
                cb->next_timeout = curtick + delay;
            }
            cb->fun = callback_fun;
            cb->data = data;
            break;
        }
    }
    if (cb) return cb->id;
    else return 0;
}

int remove_timer(uint32_t id) {
    if (id == 0 && id > MAX_CALLBACK_ID)
        return CLOCK_R_FAIL;
    if (!callback_arr[id].valid)
        return CLOCK_R_FAIL;
    callback_arr[id].valid = false;
    return CLOCK_R_OK;
}

int timer_interrupt(void) {
    timestamp_t cur_tick = time_stamp();
    bool overflowed = cur_tick < last_tick;

    sync_acquire(callback_m); 
    for (int i = 1; i <= MAX_CALLBACK_ID; i++) {
        callback_t *p = &callback_arr[i];
        if (overflowed || p->next_timeout <= cur_tick) {
            p->fun(p->id, p->data);
            if (MAX_TICK - cur_tick < p->delay)
                p->next_timeout = MAX_TICK;
            else
                p->next_timeout = cur_tick + p->delay;
        }
    }
    sync_release(callback_m);

    last_tick = cur_tick;

    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    return 0;
}

int stop_timer(void) {
    sync_destroy_mutex(callback_m);
    initialized = false;
    last_tick = 0;
    return 0;
}
