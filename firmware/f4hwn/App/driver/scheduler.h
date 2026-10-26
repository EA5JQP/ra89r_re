/* The K1's periodic countdowns, on this board (see NOTICE).
 *
 * The K1 runs these inside SysTick_Handler (App/scheduler.c).  This port's
 * SysTick handler only counts milliseconds, so `scheduler_tick_10ms()` is the
 * K1's body moved behind a call the handler makes every tenth tick.  Without it
 * the scan starts and never steps -- `gScanPauseDelayIn_10ms` is what re-arms
 * `gScheduleScanListen`, and nothing decremented it.
 */
#ifndef DRIVER_SCHEDULER_H
#define DRIVER_SCHEDULER_H

/* One 10 ms tick of the K1's countdowns (and, every fiftieth, its 500 ms
 * group).  Called from the SysTick handler; keep it short. */
void scheduler_tick_10ms(void);

#endif /* DRIVER_SCHEDULER_H */
