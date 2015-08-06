static struct gpt_register gpt_reg;
static uint32_t tick_count = 0;
static struct timer_callback callbacks[CALLBACK_ARRAY_LENGTH];
static seL4_CPtr timer_cap;

static void init_gpt_timer(seL4_CPtr ) {
    map_device(0x2098000, sizeof(struct gpt_register));
}

int start_timer(seL4_CPtr interrupt_ep) {
    timer_cap = enable_irq(GPT_IRQ, interrupt_ep);
    if (gpt_reg) 
    memset(gpt_reg, 0, sizeof(gpt_reg));
    
    // map_device
    // 
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
