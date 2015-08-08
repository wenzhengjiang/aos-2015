#include <stdio.h>
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

struct callback {
    bool valid;
    uint32_t id;
    uint64_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
};

typedef struct callback callback_t;

static const timestamp_t MAX_TICK = (1ULL<<63) - 1;

static callback_t callback_arr[MAX_CALLBACK_ID+1];
static sync_mutex_t callback_m;

static bool initialized;

static timestamp_t last_tick;

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
    if (!(callback_m = sync_create_mutex())) 
        return CLOCK_R_FAIL;

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
    sync_destroy_mutex(callback_m);
    initialized = false;
    last_tick = 0;
    return 0;
}
