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

#define FO1 1
#define OM1 4
#define FRR 0
#define CLKSRC 1
#define ENMOD 1
#define ENABLE 1
#define DISABLE 1
#define PRESCALER 66
#define IF 1
#define IF1IE 1
#define OCR1 10000

#define GPT_IRQ 87

typedef uint64_t timestamp_t;
typedef void (*timer_callback_t)(uint32_t id, void *data);

struct gpt_register {
    uint32_t control;
    uint32_t prescaler;
    uint32_t status;
    uint32_t interrupt;
    uint32_t output_compare1;
    uint32_t output_compare2;
    uint32_t output_compare3;
    uint32_t input_capture1;
    uint32_t input_capture2;
    uint32_t counter;
};

struct timer_callback {
    uint32_t  id;
    uint64_t  next_timeout;
    timer_callback_t callback;
    void *data;
};

#define CALLBACK_ARRAY_LENGTH 1024

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
