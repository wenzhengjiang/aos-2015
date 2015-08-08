#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <clock/clock.h>
#include <cspace/cspace.h>
#include <sel4/types.h>
#include <assert.h>
#include <string.h>
//#include <sync/mutex.h>

static const timestamp_t max_tick = (1ULL<<63) - 1;

struct callback {
    uint32_t id;
    uint64_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
    struct callback *next;
};
typedef struct callback callback_t;

static callback_t *callback_list;
//static sync_mutex_t callback_m;

static uint32_t gid;
//static sync_mutex_t gid_m;

static bool initialized;

static timestamp_t last_tick;

/* 
 * GPT Clock Register Settings
 */

/* FORCE OUTPUT COMPARE on Output Compare 1 */
const unsigned CLOCK_GPT_CR_FO1 = 1;
/* Set the mode of comparison for output compare 1.  Active low-pulse mode is
 * configured. */
const unsigned CLOCK_GPT_CR_OM1 = 4;

/** FREE RUN OR RESTART MODE.  0 sets Restart mode that causes the counter to
 * reset when a compare is successful */
const unsigned CLOCK_GPT_CR_FRR = 0;

/* CLOCK SOURCE: 001 - Peripheral Clock */
const unsigned CLOCK_GPT_CR_CLKSRC = 1;

/* Enable mode */
const unsigned CLOCK_GPT_CR_ENMOD = 1;

/* Enable / disabled state */
const unsigned CLOCK_GPT_CR_ENABLE = 1;
const unsigned CLOCK_GPT_CR_DISABLE = 0;

/* Free run or return restart mode */
const unsigned CLOCK_GPT_CR_FRR_RESTART = 0;

/* As the prescaler uses values 1..4096, where zero is illegal, they shift the
 * value mapping by one.  So need to subtract 1 to produce the desired value. */
const unsigned CLOCK_GPT_PRESCALER = (66 - 1);

/* GPT Interrupt register behaviours for output compare */
const unsigned CLOCK_GPT_IR_OF1IE = 1;
const unsigned CLOCK_GPT_IR_OF2IE = 0;
const unsigned CLOCK_GPT_IR_OF3IE = 0;

// TODO: How do we ensure the compiler doesn't modify the struct layout?
struct gpt_control_register {
    unsigned enable : 1;
    unsigned enable_mode: 1;
    unsigned debug_mode : 1;
    unsigned wait_mode : 1;
    unsigned doze_mode : 1;
    unsigned stop_mode_enabled : 1;
    unsigned clock_source : 3;
    unsigned free_run_or_restart : 1;
    unsigned reserved : 4;
    unsigned software_reset : 1;
    unsigned input_operating_mode_1 : 2;
    unsigned input_operating_mode_2 : 2;
    unsigned output_compare_mode_1 : 3;
    unsigned output_compare_mode_2 : 3;
    unsigned output_compare_mode_3 : 3;
    unsigned force_output_compare_1 : 3;
    unsigned force_output_compare_2 : 3;
    unsigned force_output_compare_3 : 3;
};

struct gpt_interrupt_register {
    unsigned output_compare_1_enable : 1;
    unsigned output_compare_2_enable : 1;
    unsigned output_compare_3_enable : 1;
    unsigned input_capture_1_enable : 1;
    unsigned input_capture_2_enable : 1;
    unsigned rollover_interrupt_enable : 1;
    unsigned reserved : 26;
};

struct timer_callback {
    uint32_t  id;
    uint64_t  next_timeout;
    timer_callback_t callback;
    void *data;
};

struct gpt_register_set* gpt_register_set;
static uint32_t tick_count = 0;
static seL4_CPtr _timer_cap = seL4_CapNull;
void* gpt_clock_addr;
static uint32_t clock_tick_count;

uint64_t get_uniq_id() {
    //sync_acquire(gid_m); 
    int ret = ++gid; // TODO better solusion ?
    //sync_release(gid_m);

    return ret;
};

/* Insert a new node while maintaining the increasing order of next_timou*/

static void cblist_add(callback_t *new) {
    //sync_acquire(callback_m); 

    callback_t *cur = callback_list, *prev = NULL;
    while (cur && cur->next_timeout <= new->next_timeout) {
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

    //sync_release(callback_m);
}

/*
 * @return  CLOCK_R_OK if item removed successfully 
 *          CLOCK_R_FAIL if item was not found */
static int cblist_remove(uint32_t id) {
    //sync_acquire(callback_m); 

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

    //sync_release(callback_m);
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

static seL4_CPtr
enable_irq(int irq, seL4_CPtr aep) {
    seL4_CPtr cap;
    int err;
    /* Create an IRQ handler */
    cap = cspace_irq_control_get_cap(cur_cspace, seL4_CapIRQControl, irq);
    assert(cap);
    /* Assign to an end point */
    err = seL4_IRQHandler_SetEndpoint(cap, aep);
    assert(!err);
    /* Ack the handler before continuing */
    err = seL4_IRQHandler_Ack(cap);
    assert(!err);
    return cap;
}

int start_timer(seL4_CPtr interrupt_ep) {
    struct gpt_control_register* gpt_control_register;
    struct gpt_interrupt_register* gpt_interrupt_register;
    //if (!(callback_m = sync_create_mutex())) 
    //    return CLOCK_R_FAIL;
    //if (!(gid_m = sync_create_mutex()))
    //    return CLOCK_R_FAIL;

    initialized = true;

    gpt_register_set = gpt_clock_addr;

    gpt_control_register = (struct gpt_control_register*)&(gpt_register_set->control);
    gpt_interrupt_register = (struct gpt_interrupt_register*)&(gpt_register_set->interrupt);
    _timer_cap = enable_irq(GPT_IRQ, interrupt_ep);
    assert(sizeof(gpt_control_register) == 4);
    /* Ensure the clock is stopped */

    /* Step 1: Disable GPT */
    gpt_control_register->enable = CLOCK_GPT_CR_DISABLE;

    /* Step 2: Disable Interrupt Register */
    gpt_register_set->interrupt = 0;

    /* Step 3: Configure Output Mode to disconnected */
    gpt_control_register->output_compare_mode_1 = 0;
    gpt_control_register->output_compare_mode_2 = 0;
    gpt_control_register->output_compare_mode_3 = 0;

    /* Step 4: Disable Input Capture Modes */
    gpt_control_register->input_operating_mode_1 = 0;
    gpt_control_register->input_operating_mode_2 = 0;

    /* Step 5: Set clock source to the desired value */
    gpt_control_register->clock_source = 2;

    /* Step 6: Assert SWR */
    gpt_control_register->software_reset = 1;

    /* Step 7: Clear GPT status register */
    gpt_register_set->status = 0;

    /* Step 8: Set ENMOD = 1 */
    gpt_control_register->enable_mode = CLOCK_GPT_CR_ENMOD;
    /* Step 8.5 other setup */

    gpt_control_register->stop_mode_enabled = 1;
    gpt_control_register->doze_mode = 0;
    gpt_control_register->wait_mode = 0;
    gpt_control_register->debug_mode = 0;

    gpt_control_register->free_run_or_restart = CLOCK_GPT_CR_FRR_RESTART;
    gpt_control_register->output_compare_mode_1 = 1;
    gpt_control_register->output_compare_mode_2 = 0;
    gpt_control_register->output_compare_mode_3 = 0;
    /*gpt_control_register->force_output_compare_1 = CLOCK_GPT_CR_FO1;*/
    gpt_register_set->output_compare_1 = 10000ul;
    gpt_register_set->prescaler = 66;

    /*gpt_interrupt_register->rollover_interrupt_enable = 1;*/

    /* Step 9: Enable GPT */
    gpt_control_register->enable = CLOCK_GPT_CR_ENABLE;

    /* Step 10: Enable IR */
    gpt_register_set->interrupt = 0xffffffff;
    return 0;
}

/* TODO: we shouldn't do this */
void clock_set_device_address(void* mapping) {
    gpt_clock_addr = mapping;
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
    callback_t * cb = (callback_t*)malloc(sizeof(callback_t));
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

    //sync_acquire(callback_m); 
    for (callback_t *p = callback_list; p; p = p->next) {
        if (overflowed || p->next_timeout <= cur_tick) {
            p->fun(p->id, p->data);
            if (max_tick - cur_tick < p->delay)
                p->next_timeout = max_tick;
            else
                p->next_timeout = cur_tick + p->delay;
        }
        if (!overflowed && p->next_timeout > cur_tick)
            break;
    }
    //sync_release(callback_m);

    last_tick = cur_tick;
    printf("Tick!\n");
    printf("current tick is %llu\n", time_stamp());
    gpt_register_set->status = 1;
    int err = seL4_IRQHandler_Ack(_timer_cap);
    assert(!err);
    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    return 0;
}

int stop_timer(void) {
    cblist_destroy();
    //sync_destroy_mutex(callback_m);
    //sync_destroy_mutex(gid_m);
    return 0;
}
