#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <clock/clock.h>
#include <cspace/cspace.h>
#include <device/mapping.h>
#include <device/mapping.h>
#include <sel4/types.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define MAX_CALLBACK_ID 20

#define CLOCK_SUBTICK_CAP 100000ul

#define BITS(n) (1ul<<(n))

#define GPT_CR_OM1 BITS(22)
#define GPT_CR_OM2 BITS(25)
#define GPT_CR_SWR BITS(15)
#define GPT_CR_FRR BITS(9)
#define GPT_CR_CLKSRC BITS(6)
#define GPT_CR_STOPEN BITS(5)
#define GPT_CR_DOZEEN BITS(4)
#define GPT_CR_WAITEN BITS(3)
#define GPT_CR_ENMOD BITS(1)
#define GPT_CR_EN BITS(0)

#define GPT_SR_ROV BITS(5)
#define GPT_SR_OF2 BITS(1)
#define GPT_SR_OF1 BITS(0)

#define GPT_IR_ROVIE BITS(5)
#define GPT_IR_OF2IE BITS(1)
#define GPT_IR_OF1IE BITS(0)

static const unsigned GPT_PRESCALER = (66 - 1);

static bool handling_interrupt = false;
gpt_register_t *gpt_reg;
static uint32_t high_count = 0; // higher bits of timer counter
static seL4_CPtr _timer_cap = seL4_CapNull;

struct callback {
    bool valid;
    uint32_t id;
    timestamp_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
};

typedef struct callback callback_t;

static tick_callback_t tick_callbacks[MAX_CALLBACK_ID+1]; /* callbacks get called every tick */
static callback_t callback_arr[MAX_CALLBACK_ID+1];       /* callback_arr contains all the callback functions */
static callback_t* ordered_callbacks[MAX_CALLBACK_ID+1]; /* ordered index on top of callbacks (increasing by timeouts) */

static timestamp_t g_cur_time; /* global reference to current time, so it can be accessed by cmp function */
static timestamp_t next_timeout; /* timeout of next callback */
static int next_cb = 0; /*idex of next callback */ 

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

/**
 * @brief Setup outcmp2, so we can have interrupt at given time
 *
 * @param t 
 */
static void update_outcmp2(timestamp_t t) {
    next_timeout = t;
    gpt_reg->ocr2 = (uint32_t)t;
    if (!(gpt_reg->ir & GPT_IR_OF2IE)) {
        gpt_reg->cr |= GPT_CR_OM2;
        gpt_reg->ir |= GPT_IR_OF2IE;
    }
}

/**
 * @brief Turn off cmp2
 */
static void disable_outcmp2(void) {
    gpt_reg->ir &= ~GPT_IR_OF2IE;
    gpt_reg->cr &= ~GPT_CR_OM2;
}

int callback_cmp(const void *a, const void *b) {
    callback_t *x = *(callback_t**)a, *y = *(callback_t**)b;
    if (!x->valid && !y->valid) return 0;
    if (!x->valid) return 1;
    if (!y->valid) return -1;
    if ((x->next_timeout - g_cur_time) < (y->next_timeout - g_cur_time))
        return -1;
    if ((x->next_timeout - g_cur_time) > (y->next_timeout - g_cur_time))
        return 1;
    if ((x->next_timeout - g_cur_time) == (y->next_timeout - g_cur_time))
        return 0;
    conditional_panic(true, "The impossible has happened");
    return 0;
}


/**
 * @brief Get ordered_callbacks and setup outcmp2  
 */
static void update_timeout(void) {
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
    /*if callback_arr is not empty, we setup the next cmp2 interrupt*/
    if (updated) {
        g_cur_time = cur_time;
        qsort(ordered_callbacks, MAX_CALLBACK_ID, sizeof(callback_t*), callback_cmp);
        next_cb = 0;
        update_outcmp2(closest_timeout);
        assert(closest_timeout == ordered_callbacks[0]->next_timeout);
    } /*otherwise turn off cmp2*/
    else {
        disable_outcmp2();
    }

}

/**
 * @brief Set up interrupt and configure GPT register
 *
 */
int start_timer(seL4_CPtr interrupt_ep) {
    static gpt_register_t* gpt_clock_addr = NULL;
    if (gpt_clock_addr == NULL) {
        gpt_clock_addr = map_device((void*)CLOCK_GPT_PADDR, sizeof(gpt_register_t));
        _timer_cap = enable_irq(GPT_IRQ, interrupt_ep);
    }
    gpt_reg = gpt_clock_addr;
    gpt_reg->cr = GPT_CR_OM1 | GPT_CR_FRR | GPT_CR_CLKSRC | GPT_CR_ENMOD ;
    gpt_reg->sr = 0;
    gpt_reg->ir = GPT_IR_ROVIE | GPT_IR_OF1IE;
    gpt_reg->pr = GPT_PRESCALER;

    gpt_reg->ocr1 = CLOCK_SUBTICK_CAP;
    gpt_reg->cr |= GPT_CR_EN;
    assert(gpt_reg->cr == 0b00000000010000000000001001000011);
    for (int i = 0; i < MAX_CALLBACK_ID; i++)
        ordered_callbacks[i] = &callback_arr[i+1];

    return 0;
}

/**
 * @brief Insert a tick_callback in tick_callback array, takes O(n) time
 *
 * @param callback_fun
 *
 * @return error code
 */
uint32_t register_tick_event(tick_callback_t callback_fun) {
    int i;
    for (i = 0; i < MAX_CALLBACK_ID + 1; i++) {
        if (tick_callbacks[i] == NULL) {
            break;
        }
    }
    if (i == MAX_CALLBACK_ID + 1) {
        ERR("Not registering new tick callback. Limit exceeded.");
        return ENOMEM;
    }
    tick_callbacks[i] = callback_fun;
    return 0;
}

/**
 * @brief add timer to callback array, takes O(n) time
 *
 * @param delay
 * @param callback_fun
 * @param data
 *
 * @return index of callback in callback array, or 0 if fail to add it
 */
uint32_t register_timer(uint64_t delay, timer_callback_t callback_fun, void *data) {
    if (callback_fun == NULL) {
        dprintf(1, "invalid callback_fun\n");
        return 0;
    }
    if (!(gpt_reg->cr & GPT_CR_EN)) {
        WARN("timer hasn't been initialised \n");
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

/**
 * @brief remove a timer, takes O(1) time
 *
 * @param id index of timer needs to be removed
 *
 * @return 
 */
int remove_timer(uint32_t id) {
    if (id == 0 && id > MAX_CALLBACK_ID)
        return CLOCK_R_FAIL;
    if (!callback_arr[id].valid)
        return CLOCK_R_FAIL;
    if (!(gpt_reg->cr & GPT_CR_EN)) {
        return CLOCK_R_UINT;
    }
    callback_arr[id].valid = false;

    update_timeout();
    return CLOCK_R_OK;
}

/**
 * Interrupt handler
 * @return status code indicating whether the action was successful
 */
int timer_interrupt(void) {
    int err;
    int i;
    handling_interrupt = true;
    /* handler tick interrupt */
    if (gpt_reg->sr & GPT_SR_OF1) {
        gpt_reg->ocr1 = time_stamp() + CLOCK_SUBTICK_CAP;
        gpt_reg->sr &= GPT_SR_OF1;
        for (i = 0; i < MAX_CALLBACK_ID; i++) {
            if (tick_callbacks[i] == NULL) {
                break;
            }
            tick_callbacks[i]();
        }
    }
    /* handler timer interrupt */
    if (gpt_reg->sr & GPT_SR_OF2) {
        assert(ordered_callbacks[next_cb]->next_timeout == next_timeout);
        for (i = 0; i < 5; i++) {
        }
        while (next_cb < MAX_CALLBACK_ID) { /* trigger all timers meet timeout */
            callback_t *c = ordered_callbacks[next_cb];
            if (c->next_timeout != next_timeout) break;
            c->fun(c->id, c->data);
            c->valid = false;
            next_cb++;
        }
        /*set cmp2 to trigger next timer in ordered_calbacks, which should has the closet timeout, as it's ordered*/
        if(next_cb < MAX_CALLBACK_ID) {
            update_outcmp2(ordered_callbacks[next_cb]->next_timeout);
        }
        else
            disable_outcmp2();

        gpt_reg->sr &= GPT_SR_OF2;
    }
    /*update high bits of counter if it's a overflow interrupt*/
    if (gpt_reg->sr & GPT_SR_ROV) {
        high_count++;
        gpt_reg->sr &= GPT_SR_ROV;
    }

    err = seL4_IRQHandler_Ack(_timer_cap);
    if (err) return err;
    /*if driver was turned off by callbacks, clear everything else*/
    if (!(gpt_reg->cr & GPT_CR_EN)) {
        err = seL4_IRQHandler_Clear(_timer_cap);
        assert(!err);
        err = cspace_delete_cap(cur_cspace, _timer_cap);
        assert(err == CSPACE_NOERROR);
    }
    handling_interrupt = false;
    return CLOCK_R_OK;
}

/**
 * Return a timestamp indicating the elapsed time since boot
 * return 64-bit timestamp
 * Timer is 64-bit and will roll over.
 */
timestamp_t time_stamp(void) {
    timestamp_t time;
    while(1) {
        uint32_t counter_low = gpt_reg->cnt;
        time = (timestamp_t)high_count << 32;
        time |= counter_low ;
        if (counter_low <= gpt_reg->cnt) {
            break;
        }
    }
    return time;
}

/**
 * Stop the timer
 * If called from within timer_interrupt, timer_interrupt will clean up later.
 */
int stop_timer(void) {
    gpt_reg->cr &= ~GPT_CR_EN;
    if (!handling_interrupt) {
        int err = seL4_IRQHandler_Clear(_timer_cap);
        assert(!err);
        err = cspace_delete_cap(cur_cspace, _timer_cap);
        assert(err == CSPACE_NOERROR);
    }
    return 0;
}
