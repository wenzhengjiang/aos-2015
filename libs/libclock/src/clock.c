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

#define CLOCK_SUBTICK_CAP 50ul

static const uint32_t GPT_MAX_TICK = -1UL;
/* 
 * GPT Clock Register Settings
 */

static const unsigned GPT_OM_LOWPULSE = 4;
static const unsigned GPT_OM_DISCONNECTED = 0;
static const unsigned GPT_FRR_FREEFUN = 1;
static const unsigned GPT_SRC_PERICLOCK = 1;

static const unsigned GPT_PRESCALER = (66 - 1);

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

struct gpt_status_register {
    unsigned output_compare_1_occurred : 1;
    unsigned output_compare_2_occurred : 1;
    unsigned output_compare_3_occurred : 1;
    unsigned input_capture_1_occurred : 1;
    unsigned input_capture_2_occurred : 1;
    unsigned rollover_occurred : 1;
    unsigned reserved : 26;
};

struct gpt_register_set* gpt_register_set;
static uint32_t high_count = 0;
static seL4_CPtr _timer_cap = seL4_CapNull;
void* gpt_clock_addr;

struct callback {
    bool valid;
    uint32_t id;
    uint32_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
};

typedef struct callback callback_t;

static callback_t callback_arr[MAX_CALLBACK_ID+1];
static sync_mutex_t callback_m;

static timestamp_t next_timeout;
static bool initialized;

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

static void enable_outcmp2(timestamp_t t) {
    struct gpt_control_register* cr_reg = (struct gpt_control_register*)&(gpt_register_set->control);
    struct gpt_interrupt_register* int_reg = (struct gpt_interrupt_register*)&(gpt_register_set->interrupt); 

    // the interval might larger than MAX_UINT32, global var next_timeout is used to
    // check whether current interrupt really means a timeout happens
    gpt_register_set->output_compare_2 = (uint32_t)t; 
    cr_reg->force_output_compare_2 = 1;
    cr_reg->output_compare_mode_2 = GPT_OM_LOWPULSE; 
    int_reg->output_compare_2_enable = 1;
    printf("output_compare_2: %u\n", gpt_register_set->output_compare_2);
} 

static void disable_outcmp2(void) {
    struct gpt_control_register* cr_reg = (struct gpt_control_register*)&(gpt_register_set->control);
    struct gpt_interrupt_register* int_reg = (struct gpt_interrupt_register*)&(gpt_register_set->interrupt); 
    int_reg->output_compare_2_enable = 0; 
    cr_reg->output_compare_mode_2 = GPT_OM_DISCONNECTED;
}

static void update_timeout() {
    timestamp_t cur_time = time_stamp();
    timestamp_t closest_timeout = cur_time - 1;
    callback_t *cb;
    bool updated = false;
    for (int i = 1; i <= MAX_CALLBACK_ID; i++) {
        cb = &callback_arr[i];
        if (cb->valid && cb->next_timeout - cur_time <= closest_timeout - cur_time){
            closest_timeout = cb->next_timeout;
            updated = true;
        }
    }

    if (updated) {
        enable_outcmp2(closest_timeout);
        next_timeout = closest_timeout;
    }
    else
        disable_outcmp2();

}

void clock_set_device_address(void* mapping) {
    gpt_clock_addr = mapping;
}

int start_timer(seL4_CPtr interrupt_ep) {
    if (!(callback_m = sync_create_mutex())) 
        return CLOCK_R_FAIL;

    gpt_register_set = gpt_clock_addr;
    struct gpt_control_register* gpt_control_register = (struct gpt_control_register*)&(gpt_register_set->control);
    _timer_cap = enable_irq(GPT_IRQ, interrupt_ep);

    assert(sizeof(gpt_control_register) == 4);
    // Ensure the clock is stopped
    gpt_control_register->enable_mode = 0;
    gpt_register_set->interrupt = 0;;
    gpt_control_register->output_compare_mode_1 = GPT_OM_DISCONNECTED;
    gpt_control_register->output_compare_mode_2 = GPT_OM_DISCONNECTED;
    gpt_control_register->output_compare_mode_3 = GPT_OM_DISCONNECTED;

    // set control register
    gpt_control_register->output_compare_mode_1 = GPT_OM_LOWPULSE;
    gpt_control_register->software_reset = 1;
    gpt_control_register->free_run_or_restart = GPT_FRR_FREEFUN;
    gpt_control_register->clock_source = GPT_SRC_PERICLOCK;
    gpt_control_register->stop_mode_enabled = 1;
    gpt_control_register->enable_mode = 1;
    gpt_control_register->doze_mode = 0;
    gpt_control_register->wait_mode = 0;
    gpt_control_register->debug_mode = 0;
    // set status register
    gpt_register_set->status = 0;

    gpt_register_set->output_compare_1 = CLOCK_SUBTICK_CAP;
    gpt_register_set->prescaler = GPT_PRESCALER;

    /* Enable GPT */
    gpt_control_register->enable = 1;

    /*  Enable IR OF1IE and rollover ?*/
    gpt_register_set->interrupt = -1;

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
    callback_t *cb = NULL;
    timestamp_t cur = time_stamp();

    for (int i = 1; i <= MAX_CALLBACK_ID; i++) {
        if (!callback_arr[i].valid) {
            cb = &callback_arr[i];
            cb->valid = true;
            cb->id = i;
            cb->delay = delay;
            // As interrupts now occur on clock equality, overflow now
            // becomes an interrupt selection concern
            cb->next_timeout = cur + delay;
            cb->fun = callback_fun;
            cb->data = data;
            break;
        }
    }
    update_timeout();
    if (cb) return cb->id;
    else return 0;
}

int remove_timer(uint32_t id) {
    if (id == 0 && id > MAX_CALLBACK_ID)
        return CLOCK_R_FAIL;
    if (!callback_arr[id].valid)
        return CLOCK_R_FAIL;
    callback_arr[id].valid = false;
    update_timeout();
    return CLOCK_R_OK;
}

int timer_interrupt(void) {
    int err;
    struct gpt_status_register *status = (struct gpt_status_register*)&(gpt_register_set->status);
    // Status register not being found set.  WIP
    if (status->output_compare_1_occurred) {
        gpt_register_set->output_compare_1 += CLOCK_SUBTICK_CAP;
        printf("current tick is %llu\n", time_stamp());
        status->output_compare_1_occurred = 0;
    }

    if (status->output_compare_2_occurred) {
        printf("output_compare_2_occurred\n");
        sync_acquire(callback_m);
        for (int i = 1; i <= MAX_CALLBACK_ID; i++) {
            callback_t *p = &callback_arr[i];
            /* We should be able to depend on equality as interrupts freeze the
             * clock */
            if (p->valid && p->next_timeout == next_timeout) {
                /* Fire and remove the callback */
                p->fun(p->id, p->data);
                remove_timer(p->id);
            }
        }
        sync_release(callback_m);
        status->output_compare_2_occurred = 0;
    }
    if (status->rollover_occurred) {
        high_count++;
        status->output_compare_3_occurred = 0;
    }

    err = seL4_IRQHandler_Ack(_timer_cap);
    assert(!err);
    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    uint32_t counter_low = gpt_register_set->counter;
    timestamp_t time = (timestamp_t)high_count << 32;
    time |= counter_low ;
    return time;
}

int stop_timer(void) {
    struct gpt_control_register *gpt_control_register;
    sync_destroy_mutex(callback_m);
    gpt_control_register = (struct gpt_control_register*)&(gpt_register_set->control);
    gpt_control_register->enable = 0;
    initialized = false;
    return 0;
}
