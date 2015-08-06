#include <stdbool.h>
#include <limits.h>
#include <clock/clock.h>
#include <sync/mutex.h>

static const timestamp_t max_tick = (1LL<<63) - 1;

struct callback {
    uint32_t id;
    uint64_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
    struct timer_callback *next;
};
typedef struct callback callback_t;

static callback_t *callback_list;
static sync_mutex_t callback_m;

static uint32_t gid;
static sync_mutex_t gid_m;

static bool initialized;

static timestamp_t last_tick;

uint64_t get_uniq_id() {
    sync_acquire(gid_m); 
    int ret = ++gid; // TODO better solusion ?
    sync_release(gid_m);

    return ret;
};

/* Insert a new node while maintaining the increasing order of next_timou*/

static void cblist_add(callback_t *new) {
    sync_acquire(callback_m); 

    callback_t *cur = callback_list, *prev = NULL;
    while (cur && cur->next_time <= new->next_time) {
        prev = cur;
        cur = cur->next;
    }
    new->next = NULL;
    if (!prev) {            // insert at head
        new->next = callback_list;
        callback_list = new;
    } else {                // insert after prev
        prev->next = new;
        new->next = cur;
    }

    sync_release(callback_m);
}

/*
 * @return  CLOCK_R_OK if item removed successfully 
 *          CLOCK_R_FAIL if item was not found */
static int cblist_remove(uint32_t id) {
    sync_acquire(callback_m); 

    callback_t *cur = callback_list, *prev = NULL;
    while (cur && cur->id != id) {
        prev = cur;
        cur = cur->next;
    }
    int ret = CLOCK_R_OK;
    if (!cur) ret = CLOCK_R_FAIL; // not found
    else {
        if (!prev)
            callback_list = callback_list->next; // remove head
        else
            prev->next = cur->next;
        free(cur);
    }

    sync_release(callback_m);
    return ret;
}

static void cblist_destroy() {
    callback_t * p = callback_list, *next = NULL;
    while (p) {
        next = p->next;
        free(p);
        p = next;
    }
}

int start_timer(seL4_CPtr interrupt_ep) {
    (void) interrupt_ep;

    if (!(callback_m = sync_create_mutex()) 
        return CLOCK_R_FAIL;
    if (!(gid_m = sync_create_mutex())
        return CLOCK_R_FAIL;

    initialized = true;
}
/*
 * Register a callback to be called after a given delay
 *    delay:  Delay time in microseconds before callback is invoked
 *    callback: Function to be called
 *    data: Custom data to be passed to callback function
 *
 * Returns 0 on failure, otherwise an unique ID for this timeout
 */

uint32_t register_timer(uint64_t delay, timer_callback_t callback_fun, void *data) {
    if (!initialized) return 0;
    callback_t * cb = (callback_t)malloc(sizeof(callback_t));
    if (!cb)
        return 0; 
    
    cb->id = get_uniq_id();
    cb->delay = delay;
    timestamp_t curtick = time_stamp();
    if (max_tick - curtick < delay) {
        cb->next_timeout = max_tick; // workaround for overflow
    } else {
        cb->next_timeout = curtick + delay;
    }
    cb->fun = callback_fun;
    cb->data = data;
    
    cblist_add(cb);

    return cb->id;
}

/*
 * Remove a previously registered callback by its ID
 *    id: Unique ID returned by register_time
 * Returns CLOCK_R_OK iff successful.
 */

int remove_timer(uint32_t id) {
    return cblist_remove(id);
}

/*
 * Handle an interrupt message sent to 'interrupt_ep' from start_timer
 *
 * Returns CLOCK_R_OK iff successful
 */

int timer_interrupt(void) {
    timestamp_t cur_tick = time_stamp();
    bool overflowed = cur_tick < last_tick;

    sync_acquire(callback_m); 
    for (callback_t *p = callback_list; p; p = p->next) {
        if (overflowed || p->next_timeout <= cur_tick) {
            p->fun(p->data);
            if (max_tick - cur_tick < p->delay)
                p->next_timeout = max_tick;
            else
                p->next_timeout = cur_tick + p->delay;
        }
        if (!overflow && p->next_timout > cur_tick)
            break;
    }
    sync_release(callback_m);

    last_tick = cur_tick;

    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    return 0;
}

int stop_timer(void) {
    cb_destory();
    sync_destroy_mutex(callback_m);
    sync_destroy_mutex(gid_m);
}
