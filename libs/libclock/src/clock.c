#include <clock.h>
#include <cspace/cspace.h>
#include <sel4/types.h>
#include <assert.h>
#include <string.h>

struct gpt_register_set* gpt_register_set;
static uint32_t tick_count = 0;
static struct timer_callback callbacks[CALLBACK_ARRAY_LENGTH];
static seL4_CPtr _timer_cap = seL4_CapNull;
void* gpt_clock_addr;
static uint32_t clock_tick_count;

/* TODO: we shouldn't do this */
void clock_set_device_address(void* mapping) {
    gpt_clock_addr = mapping;
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
    struct gpt_control_register* gpt_control_register = &(gpt_register_set->control);
    struct gpt_interrupt_register* gpt_interrupt_register = &(gpt_register_set->interrupt);

    gpt_register_set = gpt_clock_addr;
    _timer_cap = enable_irq(GPT_IRQ, interrupt_ep);

    assert(sizeof(gpt_control_register) == 4);

    /* Ensure the clock is stopped */
    gpt_control_register->enable = CLOCK_GPT_CR_DISABLE;

    /* Configure the clock */
    gpt_control_register->enable_mode = CLOCK_GPT_CR_ENMOD;
    memcpy(gpt_control_register->clock_source, &CLOCK_GPT_CR_OM1, 3);
    gpt_control_register->free_run_or_restart = CLOCK_GPT_CR_FRR_RESTART;
    memcpy(gpt_control_register->output_compare_mode_1, &CLOCK_GPT_CR_OM1, 3);
    gpt_control_register->force_output_compare_1 = CLOCK_GPT_CR_FO1;

    gpt_register_set->prescaler = CLOCK_GPT_PRESCALER;

    gpt_interrupt_register->output_compare_1_enable = CLOCK_GPT_IR_OF1IE;
    gpt_interrupt_register->output_compare_2_enable = CLOCK_GPT_IR_OF2IE;
    gpt_interrupt_register->output_compare_3_enable = CLOCK_GPT_IR_OF3IE;

    assert(gpt_interrupt_register->reserved == 0);

    /* Enable the clock */
    gpt_control_register->enable = CLOCK_GPT_CR_ENABLE;
}

/*
 * Register a callback to be called after a given delay
 *    delay:  Delay time in microseconds before callback is invoked
 *    callback: Function to be called
 *    data: Custom data to be passed to callback function
 *
 * Returns 0 on failure, otherwise an unique ID for this timeout
 */
uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
}

/*
 * Remove a previously registered callback by its ID
 *    id: Unique ID returned by register_time
 * Returns CLOCK_R_OK iff successful.
 */
int remove_timer(uint32_t id) {
}

/*
 * Handle an interrupt message sent to 'interrupt_ep' from start_timer
 *
 * Returns CLOCK_R_OK iff successful
 */
int timer_interrupt(void) {
}

/*
 * Returns present time in microseconds since booting.
 *
 * Returns a negative value if failure.
 */
timestamp_t time_stamp(void) {
}

/*
 * Stop clock driver operation.
 *
 * Returns CLOCK_R_OK iff successful.
 */
int stop_timer(void) {

}
