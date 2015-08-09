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
static uint32_t high_count = 0;
static seL4_CPtr _timer_cap = seL4_CapNull;
void* gpt_clock_addr;

struct callback {
    bool valid;
    uint32_t id;
    timestamp_t next_timeout;
    uint64_t delay;
    timer_callback_t fun;
    void *data;
};

typedef struct callback callback_t;

static callback_t callback_arr[MAX_CALLBACK_ID+1];
static sync_mutex_t callback_m;

static timestamp_t next_timeout;

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

static void update_outcmp2(timestamp_t t) {
    gpt_reg->ocr2 = (uint32_t)t;
    if (!(gpt_reg->ir & GPT_IR_OF2IE)) {
        gpt_reg->cr |= GPT_CR_OM2;
        gpt_reg->ir |= GPT_IR_OF2IE;
    }
}

static void disable_outcmp2(void) {
    gpt_reg->ir &= ~GPT_IR_OF2IE;
    gpt_reg->cr &= ~GPT_CR_OM2;
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
        update_outcmp2(closest_timeout);
        next_timeout = closest_timeout;
        printf("next timeout: %llu\n", next_timeout);
    }
    else {
        disable_outcmp2();
        printf("no timer\n");
    }

}

void clock_set_device_address(void* mapping) {
    gpt_clock_addr = mapping;
}

void debug_bits(uint32_t i) {
    for (int k = 31; k >= 0; k--) {
        if (i & (1<<k))
            putchar('1');
        else putchar('0');
    }
    putchar('\n');
}

int start_timer(seL4_CPtr interrupt_ep) {
    if (!(callback_m = sync_create_mutex())) {
        return CLOCK_R_FAIL;
    }

    gpt_reg = gpt_clock_addr;
    _timer_cap = enable_irq(GPT_IRQ, interrupt_ep);
    gpt_reg->cr = GPT_CR_OM1 | GPT_CR_FRR | GPT_CR_CLKSRC | GPT_CR_ENMOD ;
    gpt_reg->sr = 0;
    gpt_reg->ir = GPT_IR_ROVIE | GPT_IR_OF1IE;
    gpt_reg->pr = GPT_PRESCALER;

    gpt_reg->ocr1 = CLOCK_SUBTICK_CAP;
    gpt_reg->cr |= GPT_CR_EN;
    // Ensure the control register is configured as expected
    assert(gpt_reg->cr == 0b00000000010000000000001001000011);
    return 0;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback_fun, void *data) {
    if (callback_fun == NULL) {
        printf("invalid callback_fun\n");
        return 0;
    }
    if (!(gpt_reg->cr & GPT_CR_EN)) {
        printf("timer hasn't been initialised \n");
        return 0;
    }
    sync_acquire(callback_m);
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
            printf("add new timer %llu, %llu, %llu\n", cur, delay, cb->next_timeout);
            break;
        }
    }
    update_timeout();
    sync_release(callback_m);
    if (cb) return cb->id;
    else return 0;
}

int remove_timer(uint32_t id) {
    if (id == 0 && id > MAX_CALLBACK_ID)
        return CLOCK_R_FAIL;
    if (!callback_arr[id].valid)
        return CLOCK_R_FAIL;
    if (!(gpt_reg->cr & GPT_CR_EN)) {
        return CLOCK_R_UINT;
    }
    sync_acquire(callback_m);
    callback_arr[id].valid = false;
    update_timeout();
    sync_release(callback_m);
    return CLOCK_R_OK;
}

/**
 * Interrupt handler
 * @return status code indicating whether the action was successful
 */
int timer_interrupt(void) {
    int err;
    handling_interrupt = true;
    if (gpt_reg->sr & GPT_SR_OF1) {
        gpt_reg->ocr1 = time_stamp() + CLOCK_SUBTICK_CAP;
        gpt_reg->sr &= GPT_SR_OF1;
        printf("OF1 interrupt. ocr1 = %u\n", gpt_reg->ocr1);
    }
    if (gpt_reg->sr & GPT_SR_OF2) {
        printf("OF2 interrupt. ocr2 = %u\n", gpt_reg->ocr2);
        for (int i = 1; i <= MAX_CALLBACK_ID; i++) {
            callback_t *p = &callback_arr[i];
            if (p->valid && p->next_timeout == next_timeout) {
                p->fun(p->id, p->data);
                callback_arr[i].valid = false;
            }
        }
        update_timeout();
        printf("ocr1 = %u\n", gpt_reg->ocr1);
        gpt_reg->sr &= GPT_SR_OF2;
    }
    if (gpt_reg->sr & GPT_SR_ROV) {
        printf("Rollover interrupt\n");
        high_count++;
        gpt_reg->sr &= GPT_SR_ROV;
    }

    err = seL4_IRQHandler_Ack(_timer_cap);
    assert(!err);
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

int stop_timer(void) {
    sync_destroy_mutex(callback_m);
    gpt_reg->cr &= ~GPT_CR_EN;
    if (!handling_interrupt) {
        int err = seL4_IRQHandler_Clear(_timer_cap);
        assert(!err);
        err = cspace_delete_cap(cur_cspace, _timer_cap);
        assert(err == CSPACE_NOERROR);
    }
    return 0;
}
