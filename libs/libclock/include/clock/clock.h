/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#ifndef _CLOCK_H_
#define _CLOCK_H_

#include <sel4/sel4.h>

/*
 * Return codes for driver functions
 */
#define CLOCK_R_OK     0        /* success */
#define CLOCK_R_UINT (-1)       /* driver not initialised */
#define CLOCK_R_CNCL (-2)       /* operation cancelled (driver stopped) */
#define CLOCK_R_FAIL (-3)       /* operation failed for other reason */

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
#define CLOCK_GPT_PADDR 0x2098000

#define GPT_IRQ 87

typedef uint64_t timestamp_t;
typedef char byte_t;
typedef void (*timer_callback_t)(uint32_t id, void *data);

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

struct gpt_register_set {
    uint32_t control;
    uint32_t prescaler;
    uint32_t status;
    uint32_t interrupt;
    uint32_t output_compare_1;
    uint32_t output_compare_2;
    uint32_t output_compare_3;
    uint32_t input_capture_1;
    uint32_t input_capture_2;
    uint32_t counter;
};


#define CALLBACK_ARRAY_LENGTH 1024

void clock_set_device_address(void* mapping);

/*
 * Initialise driver. Performs implicit stop_timer() if already initialised.
 *    interrupt_ep:       A (possibly badged) async endpoint that the driver
 should use for deliverying interrupts to
 *
 * Returns CLOCK_R_OK iff successful.
 */
int start_timer(seL4_CPtr interrupt_ep);

/*
 * Register a callback to be called after a given delay
 *    delay:  Delay time in microseconds before callback is invoked
 *    callback: Function to be called
 *    data: Custom data to be passed to callback function
 *
 * Returns 0 on failure, otherwise an unique ID for this timeout
 */
uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data);

/*
 * Remove a previously registered callback by its ID
 *    id: Unique ID returned by register_time
 * Returns CLOCK_R_OK iff successful.
 */
int remove_timer(uint32_t id);

/*
 * Handle an interrupt message sent to 'interrupt_ep' from start_timer
 *
 * Returns CLOCK_R_OK iff successful
 */
int timer_interrupt(void);

/*
 * Returns present time in microseconds since booting.
 *
 * Returns a negative value if failure.
 */
timestamp_t time_stamp(void);

/*
 * Stop clock driver operation.
 *
 * Returns CLOCK_R_OK iff successful.
 */
int stop_timer(void);

#endif /* _CLOCK_H_ */
