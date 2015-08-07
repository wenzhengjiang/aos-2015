#include <clock/clock.h>
#include <cspace/cspace.h>
#include <sel4/types.h>
#include <assert.h>
#include <string.h>

#define CALLBACK_ARRAY_LENGTH 1024

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

/* The physical starting address of the GPT for memory mapping */
typedef char byte_t;

// TODO: How do we ensure the compiler doesn't modify the struct layout?
struct gpt_control_register {
    byte_t enable;
    byte_t enable_mode;
    byte_t debug_mode;
    byte_t wain_mode;
    byte_t doze_mode;
    byte_t stop_mode_enabled;
    byte_t clock_source[3];
    byte_t free_run_or_restart;
    byte_t reserved[4];
    byte_t software_reset;
    byte_t input_operating_mode_1[2];
    byte_t input_operating_mode_2[2];
    byte_t output_compare_mode_1[3];
    byte_t output_compare_mode_2[3];
    byte_t output_compare_mode_3[3];
    byte_t force_output_compare_1;
    byte_t force_output_compare_2;
    byte_t force_output_compare_3;
};

struct gpt_interrupt_register {
    byte_t output_compare_1_enable;
    byte_t output_compare_2_enable;
    byte_t output_compare_3_enable;
    byte_t input_capture_1_enable;
    byte_t input_capture_2_enable;
    byte_t rollover_interrupt_enable;
    byte_t reserved[26];
};

struct timer_callback {
    uint32_t  id;
    uint64_t  next_timeout;
    timer_callback_t callback;
    void *data;
};

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
