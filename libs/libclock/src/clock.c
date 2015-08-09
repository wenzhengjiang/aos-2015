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

#define CLOCK_SUBTICK_CAP 1000000ul

#define BITS(n) (1ul<<(n))

#define GPT_CR_OM1 BITS(22)
#define GPT_CR_OM2 BITS(25)
#define GPT_CR_FRR BITS(9)
#define GPT_CR_CLKSRC BITS(6)
#define GPT_CR_ENMOD BITS(1)
#define GPT_CR_EN BITS(0)

#define GPT_SR_ROV BITS(5)
#define GPT_SR_OF2 BITS(1)
#define GPT_SR_OF1 BITS(0)

#define GPT_IR_ROVIE BITS(5)
#define GPT_IR_OF2IE BITS(1)
#define GPT_IR_OF1IE BITS(0)

static const unsigned GPT_PRESCALER = (66 - 1);

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
        dprintf(0, "next timeout: %llu\n", next_timeout);
    }
    else {
        disable_outcmp2();
        dprintf(0, "no timer\n");
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
    if (!(callback_m = sync_create_mutex())) 
        return CLOCK_R_FAIL;


    gpt_reg = gpt_clock_addr;
    _timer_cap = enable_irq(GPT_IRQ, interrupt_ep);
    gpt_reg->cr = GPT_CR_OM1 | GPT_CR_FRR | GPT_CR_CLKSRC | GPT_CR_ENMOD;
    gpt_reg->sr = 0;
    gpt_reg->ir = GPT_IR_ROVIE | GPT_IR_OF1IE;
    gpt_reg->pr = GPT_PRESCALER;

    gpt_reg->ocr1 = CLOCK_SUBTICK_CAP;  
    gpt_reg->cr |= GPT_CR_EN;
    
    debug_bits(gpt_reg->cr);
    assert(gpt_reg->cr == 0b00000000010000000000001001000011);
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
            cb->next_timeout = cur + delay;
            cb->fun = callback_fun;
            cb->data = data;
            //printf("add new timer %llu, %llu, %llu\n", cur, delay, cb->next_timeout);
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
    //printf("enter timer_interrupt\n");
    int err;
    if (gpt_reg->sr & GPT_SR_OF1) {
        gpt_reg->ocr1 += CLOCK_SUBTICK_CAP;
        gpt_reg->sr &= ~GPT_SR_OF1;
    }

    if (gpt_reg->sr & GPT_SR_OF2) {
        for (int i = 1; i <= MAX_CALLBACK_ID; i++) {
            callback_t *p = &callback_arr[i];
            if (p->valid && p->next_timeout == next_timeout) {
                p->fun(p->id, p->data);
                callback_arr[i].valid = false;
            }
        }
        //printf("before update_timeout\n");
        update_timeout();
        //printf("after update_timeout\n");
        gpt_reg->sr &= ~GPT_SR_OF2;
    }
    if (gpt_reg->sr & GPT_SR_ROV) {
        high_count++;
        gpt_reg->sr &= ~GPT_SR_ROV;
    }

    err = seL4_IRQHandler_Ack(_timer_cap);
    assert(!err);
    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    uint32_t counter_low = gpt_reg->cnt;
    timestamp_t time = (timestamp_t)high_count << 32;
    time |= counter_low ;
    return time;
}

int stop_timer(void) {
    //printf("stop_timer called\n");
    sync_destroy_mutex(callback_m);
    gpt_reg->cr &= ~GPT_CR_EN;

    int err = seL4_IRQHandler_Clear(_timer_cap);
    assert(!err);
    err = cspace_delete_cap(cur_cspace, _timer_cap);
    assert(err == CSPACE_NOERROR);

    initialized = false;
    return 0;
}
