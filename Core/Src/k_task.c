/*
 * k_task.c
 *
 * Task management and EDF scheduler implementation.
 */

#include "k_task.h"
#include "k_mem.h"
#include "stm32f4xx.h"
#include <string.h>

/* round x up to the nearest multiple of 8 (stack alignment requirement) */
#define ALIGN8(x)   (((x) + 7u) & ~7u)

//kernel states
static TCB g_tcbs[MAX_TASKS]; //tcb slots
static U8 g_inited = 0; //set after oskernel init  /* set after osKernelInit */
static U8 g_running = 0; //set after kernel starts
static task_t g_current_tid = TID_NULL;

// ms counter -> increment every interrupt.
static volatile U32 g_tick_count = 0;

// NULL task shoudl get statically allocated stack 
static uint8_t g_null_stack[STACK_SIZE] __attribute__((aligned(8)));


static void null_task(void *args);
static task_t edf_pick_next(void);
static int k_osCreateTask_internal(int deadline_ms, TCB *task);


/* =========================================================================
 * SVC wrappers: functionality
 *
 * Each one just shoves arguments into the right registers and fires an SVC.
 * The SVC number tells SVC_Handler_Main which kernel function to call.
 * We need the "register ... __asm("rN")" trick to control which physical
 * register holds each argument, since the SVC handler reads them out of
 * the exception frame by index (svc_args[0] = r0, etc.).
 * ========================================================================= */

void osKernelInit(void) {
    __asm volatile ("svc %0" : : "I"(SVC_KINIT) : "memory");
}

int osCreateTask(TCB *task) {
    register int r0 __asm("r0") = (int)task;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_CREATE) : "memory");
    return r0;
}

int osKernelStart(void) {
    register int r0 __asm("r0") = 0;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_START) : "memory");
    return r0;
}

void osYield(void) {
    __asm volatile ("svc %0" : : "I"(SVC_YIELD) : "memory");
}

int osTaskInfo(task_t TID, TCB *out) {
    register int  r0 __asm("r0") = (int)TID;
    register TCB *r1 __asm("r1") = out;
    __asm volatile ("svc %2" : "+r"(r0) : "r"(r1), "I"(SVC_TASKINFO) : "memory");
    return r0;
}

task_t osGetTID(void) {
    register task_t r0 __asm("r0") = 0;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_GETTID) : "memory");
    return r0;
}

int osTaskExit(void) {
    register int r0 __asm("r0") = 0;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_EXIT) : "memory");
    return r0;
}

void osSleep(int timeInMs) {
    register int r0 __asm("r0") = timeInMs;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_SLEEP) : "memory");
}

void osPeriodYield(void) {
    __asm volatile ("svc %0" : : "I"(SVC_PERIOD_YIELD) : "memory");
}

int osSetDeadline(int deadline, task_t TID) {
    register int    r0 __asm("r0") = deadline;
    register task_t r1 __asm("r1") = TID;
    __asm volatile ("svc %2" : "+r"(r0) : "r"(r1), "I"(SVC_SET_DEADLINE) : "memory");
    return r0;
}

int osCreateDeadlineTask(int deadline, TCB *task) {
    register int  r0 __asm("r0") = deadline;
    register TCB *r1 __asm("r1") = task;
    __asm volatile ("svc %2" : "+r"(r0) : "r"(r1), "I"(SVC_CREATE_DEADLINE) : "memory");
    return r0;
}


/* =========================================================================
 * k_osKernelInit
 *
 * Zero everything out, set interrupt priorities, build the NULL task.
 * Must be called before anything else.
 * ========================================================================= */
void k_osKernelInit(void)
{
    memset(g_tcbs, 0, sizeof(g_tcbs));
    g_inited = 1;
    g_running = 0;
    g_current_tid = TID_NULL;
    g_tick_count = 0;

    /* PendSV needs to be the lowest priority interrupt in the system.
     * If SysTick could pre-empt PendSV we'd get a context switch interrupted
     * by another context switch trigger, which would be a bad time.
     * 0xFF = lowest possible priority on Cortex-M4. */
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    /* SysTick sits just above PendSV so it can interrupt running tasks
     * and pend a switch if needed, but can't interrupt PendSV itself */
    NVIC_SetPriority(SysTick_IRQn, 0xFE);

    /* put every slot in a clean dormant state */
    for (task_t i = 0; i < MAX_TASKS; i++) {
        g_tcbs[i].tid  = i;
        g_tcbs[i].state = DORMANT;
        g_tcbs[i].psp = 0;
        g_tcbs[i].args = NULL;
        g_tcbs[i].ptask = NULL;
        g_tcbs[i].stack_high = 0;
        g_tcbs[i].stack_size = 0;
        g_tcbs[i].deadline = DEFAULT_DEADLINE_MS;
        g_tcbs[i].time_remaining  = DEFAULT_DEADLINE_MS;
        g_tcbs[i].sleep_remaining = 0;
        g_tcbs[i].stack_dynamic = 0;
        g_tcbs[i].period_start = 0;
    }

    /* set up the NULL task (TID 0) -- this is the idle task that runs
     * whenever no user tasks are ready. It lives on a static stack because
     * the heap allocator hasn't been initialized yet at this point. */
    g_tcbs[TID_NULL].ptask = null_task;
    g_tcbs[TID_NULL].args = NULL;
    g_tcbs[TID_NULL].tid = TID_NULL;
    g_tcbs[TID_NULL].state = READY;
    g_tcbs[TID_NULL].stack_size = STACK_SIZE;
    g_tcbs[TID_NULL].stack_high = (U32)(g_null_stack + STACK_SIZE);
    g_tcbs[TID_NULL].deadline = DEFAULT_DEADLINE_MS;
    g_tcbs[TID_NULL].time_remaining = DEFAULT_DEADLINE_MS;
    g_tcbs[TID_NULL].stack_dynamic = 0;
    g_tcbs[TID_NULL].period_start = 0;

    /*
     * Manually build the initial exception frame for the NULL task.
     *
     * When PendSV does its first ldmia to "restore" context, it expects
     * to find r4-r11 at the top of the stack (those are the callee-saved
     * regs we push/pop ourselves), followed by the hardware-saved frame:
     *   r0, r1, r2, r3, r12, lr, pc, xpsr
     * (see ARMv7-M Architecture Reference Manual, B1.5.6)
     *
     * We push them in reverse order since the stack grows downward.
     */
    U32 *sp = (U32 *)(g_null_stack + STACK_SIZE);
    *(--sp) = 0x01000000u; /* xPSR: Thumb bit must be set, everything else 0 */
    *(--sp) = (U32)null_task; /* PC: where execution starts */
    *(--sp) = 0xFFFFFFFDu;    /* LR: EXC_RETURN value -- return to thread mode, use PSP */
    *(--sp) = 0u;             /* R12 */
    *(--sp) = 0u;             /* R3  */
    *(--sp) = 0u;             /* R2  */
    *(--sp) = 0u;             /* R1  */
    *(--sp) = 0u;             /* R0  (no args for null task) */
    /* callee-saved r4-r11, all zeroed out */
    *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u;
    *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u;
    g_tcbs[TID_NULL].psp = (U32)sp;
}


/* =========================================================================
 * k_osCreateTask_internal
 *
 * Shared implementation for both osCreateTask and osCreateDeadlineTask.
 * Finds a free TCB slot, allocates a stack from the heap, builds the initial
 * exception frame, and (if the kernel is running) triggers a context switch
 * if the new task is more urgent than whatever is currently running.
 * ========================================================================= */
static int k_osCreateTask_internal(int deadline_ms, TCB *task)
{
    if (!g_inited || task == NULL)
        return RTX_ERR;

    /* round stack size up to 8-byte boundary -- Cortex-M4 ABI requires
     * 8-byte stack alignment at exception entry (AAPCS section 5.2.1.2) */
    U32 req_size = ALIGN8((U32)task->stack_size);

    if (req_size < STACK_SIZE || task->ptask == NULL)
        return RTX_ERR;

    if (deadline_ms <= 0)
        return RTX_ERR;

    /* find the first free slot -- TID 0 is always the NULL task so skip it */
    task_t tid = TID_NULL;
    for (task_t i = 1; i < MAX_TASKS; i++) {
        if (g_tcbs[i].state == DORMANT) { tid = i; break; }
    }
    if (tid == TID_NULL) return RTX_ERR; /* all slots taken */

    /* allocate the stack from our heap. We call k_k_mem_alloc directly
     * instead of going through the SVC wrapper because we're already inside
     * an SVC handler -- a nested SVC would escalate to a HardFault */
    uint8_t *stack_mem = (uint8_t *)k_k_mem_alloc(req_size);
    if (stack_mem == NULL) return RTX_ERR;

    /* fill in the TCB */
    g_tcbs[tid].ptask          = task->ptask;
    g_tcbs[tid].args           = task->args;
    g_tcbs[tid].tid            = tid;
    g_tcbs[tid].state          = READY;
    g_tcbs[tid].stack_size     = (U16)req_size;
    g_tcbs[tid].stack_high     = (U32)(stack_mem + req_size);
    g_tcbs[tid].deadline       = (U32)deadline_ms;
    g_tcbs[tid].time_remaining = (U32)deadline_ms;
    g_tcbs[tid].sleep_remaining= 0;
    g_tcbs[tid].stack_dynamic  = 1;
    /* anchor this task's first period to the current tick so its first
     * osPeriodYield sleeps for a full period rather than a random fragment */
    g_tcbs[tid].period_start   = g_tick_count;

    // same initial stack frame layout as the NULL task above
    U32 *sp = (U32 *)(stack_mem + req_size);
    *(--sp) = 0x01000000u;              /* xPSR */
    *(--sp) = (U32)(g_tcbs[tid].ptask); /* PC   */
    *(--sp) = 0xFFFFFFFDu;              /* LR   */
    *(--sp) = 0u;                       /* R12  */
    *(--sp) = 0u;                       /* R3   */
    *(--sp) = 0u;                       /* R2   */
    *(--sp) = 0u;                       /* R1   */
    *(--sp) = (U32)(g_tcbs[tid].args);  /* R0   */
    /* callee-saved r4-r11 */
    *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u;
    *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u; *(--sp) = 0u;
    g_tcbs[tid].psp = (U32)sp;

    /* let the caller know what TID was assigned */
    task->tid = tid;

    /* if the kernel is already running, check if the new task should
     * pre-empt the current one. We compare time_remaining (not deadline)
     * because the running task may have already burned through part of its
     * timeslice, so its effective urgency right now is time_remaining, not
     * the full deadline value */
    if (g_running) {
        if (g_tcbs[tid].time_remaining < g_tcbs[g_current_tid].time_remaining) {
            SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
        }
    }

    return RTX_OK;
}


/* osCreateTask uses the default 5ms deadline from common.h */
int k_osCreateTask(TCB *task)
{
    return k_osCreateTask_internal(DEFAULT_DEADLINE_MS, task);
}
/*
 * k_osKernelStart
 *
 * Called once after all tasks are created. Resets all the timers, picks the
 * first task to run via EDF, and fires PendSV to do the first context load. */
int k_osKernelStart(void)
{
    if (!g_inited || g_running)
        return RTX_ERR;

    g_running = 1;

    /* reset SysTick's current value so the first tick is exactly 1ms away,
     * then re-anchor our ms counter to this moment. Without this, however
     * many ticks accumulated between HAL_Init and here would throw off all
     * the period boundaries for the first iteration */
    SysTick->VAL = 0;
    g_tick_count = 0;

    /* give every ready task a fresh time_remaining and anchor its period
     * to tick 0 so all period boundaries are clean multiples of each
     * task's deadline from kernel start */
    for (task_t i = 0; i < MAX_TASKS; i++) {
        if (g_tcbs[i].state == READY || g_tcbs[i].state == RUNNING) {
            g_tcbs[i].time_remaining = g_tcbs[i].deadline;
            g_tcbs[i].period_start = g_tick_count;
        }
    }

    g_current_tid = edf_pick_next();
    g_tcbs[g_current_tid].state = RUNNING;

    /* pend a PendSV -- it will load the first task's context and return to it */
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;

    return RTX_OK;
}


/* k_osYield
 *
 * Cooperative yield. The calling task stays READY but gives up the CPU.
 * We reset its time_remaining to the full deadline so it goes to the back
 * of the EDF queue relative to tasks with equal deadlines that haven't
 * yielded yet -- otherwise it would just win again immediately.*/
void k_osYield(void)
{
    if (!g_running) return;
    g_tcbs[g_current_tid].time_remaining = g_tcbs[g_current_tid].deadline;
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}


/*k_osTaskInfo / k_osGetTID */

int k_osTaskInfo(task_t TID, TCB *task_copy)
{
    if (!g_inited || task_copy == NULL) return RTX_ERR;
    if (TID >= MAX_TASKS || g_tcbs[TID].state == DORMANT) return RTX_ERR;
    *task_copy = g_tcbs[TID]; /* shallow copy should work??*/
    return RTX_OK;
}

task_t k_osGetTID(void)
{
    if (!g_running) return TID_NULL;
    return g_current_tid;
}


/* k_osTaskExit
 *
 * Terminates the calling task, frees its stack, and triggers a reschedule.
 *
 * The tricky part: the stack was allocated by whichever task called
 * osCreateTask/osCreateDeadlineTask, so k_mem.c recorded *that* task's TID
 * as the owner of the memory block. But we need the exiting task to free it.
 * We patch the owner byte in the block header directly before calling
 * k_k_mem_dealloc so the ownership check passes. */
int k_osTaskExit(void)
{
    if (!g_running) return RTX_ERR;

    task_t tid = g_current_tid;

    if (g_tcbs[tid].stack_dynamic) {
        uint8_t *stack_base = (uint8_t *)(g_tcbs[tid].stack_high - g_tcbs[tid].stack_size);

        /* BlockHeader layout in k_mem.c:
         *   bytes [0..3]  = uint32_t size
         *   byte  [4] = uint8_t  is_free
         *   byte  [5] = uint8_t  owner   <-- we patch this
         *   bytes [6..7] = uint16_t _pad
         *   bytes [8..11] = ptr next_free
         * header is the 12 bytes immediately before the usable block */
        uint8_t *blk_hdr = stack_base - 12u;
        blk_hdr[5] = (uint8_t)tid; /* claim ownership so dealloc doesn't reject us */
        k_k_mem_dealloc((void *)stack_base);
    }

    /* wipe the TCB so this slot can be reused */
    g_tcbs[tid].state = DORMANT;
    g_tcbs[tid].psp = 0;
    g_tcbs[tid].ptask = NULL;
    g_tcbs[tid].args = NULL;
    g_tcbs[tid].stack_high = 0;
    g_tcbs[tid].stack_size = 0;
    g_tcbs[tid].stack_dynamic = 0;
    g_tcbs[tid].time_remaining = 0;
    g_tcbs[tid].sleep_remaining= 0;
    g_tcbs[tid].period_start = 0;

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    return RTX_OK;
}


/*  k_osSleep
 *
 * Put the calling task to sleep for a fixed number of ms. SysTick will
 * count down sleep_remaining and wake it back up when it hits zero.*/
void k_osSleep(int timeInMs)
{
    if (!g_running || timeInMs <= 0) return;

    g_tcbs[g_current_tid].state           = SLEEPING;
    g_tcbs[g_current_tid].sleep_remaining = (U32)timeInMs;

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}


/* k_osPeriodYield*/
void k_osPeriodYield(void)
{
    if (!g_running) return;

    task_t tid = g_current_tid;
    U32 next_boundary = g_tcbs[tid].period_start + g_tcbs[tid].deadline;
    U32 now = g_tick_count;

    U32 sleep_ms;
    if (next_boundary > now)
        sleep_ms = next_boundary - now;
    else
        sleep_ms = 1; /* missed our deadline window, yield minimally and reschedule */

    /* slide the period anchor forward for the next call */
    g_tcbs[tid].period_start = next_boundary;
    g_tcbs[tid].state = SLEEPING;
    g_tcbs[tid].sleep_remaining = sleep_ms;

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}


/* k_osSetDeadline
 *
 * Permanently changes the deadline of a READY task (not the caller itself).
 * If the target task ends up more urgent than the currently running task
 * after the change, we trigger a pre-emptive context switch.
 *
 * Important subtlety: we also reset period_start to now. If we didn't,
 * the target task's next osPeriodYield would compute next_boundary using
 * the OLD period_start, which could be way in the past (especially if we're
 * shrinking the deadline), causing it to either wake up immediately or sleep
 * for a garbage value. Resetting period_start gives it a clean start from
 * the moment its deadline was changed.
 *
 * We disable interrupts around the write so SysTick can't tick and
 * decrement time_remaining between our reads and writes and leave things
 * in an inconsistent state. */

int k_osSetDeadline(int deadline, task_t TID)
{
    if (deadline <= 0) return RTX_ERR;
    if (TID >= MAX_TASKS) return RTX_ERR;
    if (TID == g_current_tid) return RTX_ERR; /* can't change your own deadline */
    if (g_tcbs[TID].state != READY) return RTX_ERR;

    __disable_irq();

    g_tcbs[TID].deadline = (U32)deadline;
    g_tcbs[TID].time_remaining = (U32)deadline;  /* urgency resets to the new value */
    g_tcbs[TID].period_start = g_tick_count;   /* re-anchor period so osPeriodYield works correctly */

    U32 target_remaining = g_tcbs[TID].time_remaining;
    U32 current_remaining = g_tcbs[g_current_tid].time_remaining;

    __enable_irq();

    /* pre-empt if the target is now more urgent than us */
    if (g_running && target_remaining < current_remaining) {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }

    return RTX_OK;
}


/* osCreateDeadlineTask just passes the custom deadline through */
int k_osCreateDeadlineTask(int deadline, TCB *task)
{
    return k_osCreateTask_internal(deadline, task);
}


/* k_tick  (called from SysTick_Handler every 1ms) (searched functionality)
 *
 * Two jobs:
 *   1. Count down sleep timers and wake tasks whose time is up.
 *   2. Count down time_remaining for all active tasks and pre-empt the
 *      running task if its timeslice expires.
 *
 * We tick time_remaining for ALL ready/running tasks simultaneously --
 * that's the key property of EDF: while one task runs, everyone else's
 * deadline is also approaching. The task with the smallest time_remaining
 * is the most urgent. */
void k_tick(void)
{
    if (!g_running) return;

    g_tick_count++; /* advance the absolute ms counter */

    U8 need_switch = 0;

    for (task_t i = 0; i < MAX_TASKS; i++) {

        if (g_tcbs[i].state == SLEEPING) {
            if (g_tcbs[i].sleep_remaining > 0)
                g_tcbs[i].sleep_remaining--;

            if (g_tcbs[i].sleep_remaining == 0) {
                /* task woke up -- give it a fresh deadline countdown */
                g_tcbs[i].state          = READY;
                g_tcbs[i].time_remaining = g_tcbs[i].deadline;
                need_switch = 1; /* newly ready task might be most urgent */
            }
        }
        else if (g_tcbs[i].state == READY || g_tcbs[i].state == RUNNING) {
            /* tick down everyone's countdown (EDF: all deadlines approach in parallel) */
            if (g_tcbs[i].time_remaining > 0)
                g_tcbs[i].time_remaining--;

            /* if the currently running task just used up its full timeslice,
             * reset and trigger a switch so the scheduler can re-evaluate */
            if (g_tcbs[i].state == RUNNING && g_tcbs[i].time_remaining == 0) {
                g_tcbs[i].time_remaining = g_tcbs[i].deadline;
                need_switch = 1;
            }
        }
    }

    if (need_switch) {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
}


/* PendSV helpers (serached functionality)
 *
 * PendSV_Handler is in stm32f4xx_it.c (written in naked asm). It calls these
 * two C functions to do the bookkeeping that's easier in C.
 *
 * Flow:
 *   1. PendSV saves r4-r11 onto PSP, then calls k_pendsv_save_psp(psp)
 *   2. k_pendsv_save_psp stores PSP in the current TCB, marks task READY
 *   3. PendSV calls k_pendsv_schedule_next(), which runs EDF and returns
 *      the new task's PSP
 *   4. PendSV restores r4-r11 from the new PSP and returns to the new task*/

void k_pendsv_save_psp(U32 psp)
{
    if (!g_running) return;
    if (g_current_tid < MAX_TASKS) {
        g_tcbs[g_current_tid].psp = psp;
        /* only move to READY if we were RUNNING */
        if (g_tcbs[g_current_tid].state == RUNNING)
            g_tcbs[g_current_tid].state = READY;
    }
}

U32 k_pendsv_schedule_next(void)
{
    if (!g_running) return 0;

    /* only re-run the scheduler if the current task actually gave up the CPU
     * (if it's still RUNNING, something weird happened and we leave it alone) */
    if (g_tcbs[g_current_tid].state == READY ||
        g_tcbs[g_current_tid].state == DORMANT ||
        g_tcbs[g_current_tid].state == SLEEPING)
    {
        g_current_tid = edf_pick_next();
    }

    g_tcbs[g_current_tid].state = RUNNING;
    return g_tcbs[g_current_tid].psp;
}

task_t k_get_current_tid(void)   { return g_current_tid; }
int    k_is_kernel_running(void) { return (int)g_running; }


/* EDF scheduler (edf_pick_next)*/
static task_t edf_pick_next(void)
{
    /* pass 1: find the minimum time_remaining among all ready user tasks */
    U32 min_tr = 0xFFFFFFFFu;
    for (task_t i = 1; i < MAX_TASKS; i++) {
        if (g_tcbs[i].state != READY) continue;
        if (g_tcbs[i].time_remaining < min_tr)
            min_tr = g_tcbs[i].time_remaining;
    }

    /* pass 2: circular scan starting after current task for fair tie-breaking */
    if (min_tr < 0xFFFFFFFFu) {
        for (task_t off = 1; off < MAX_TASKS; off++) {
            task_t i = (g_current_tid + off) % MAX_TASKS;
            if (i == TID_NULL) continue; /* skip the null task in this pass */
            if (g_tcbs[i].state == READY && g_tcbs[i].time_remaining == min_tr)
                return i;
        }
    }

    /* no user tasks ready -- fall back to the NULL/idle task */
    if (g_tcbs[TID_NULL].state == READY) return TID_NULL;

    /* shouldn't get here, but just in case, stay on current */
    return g_current_tid;
}


/* NULL task (idle task)
 *
 * Runs when everything else is sleeping. Just yields in a loop so the
 * scheduler gets called each time a task might have woken up.*/
static void null_task(void *args)
{
    (void)args;
    while (1) {
        osYield();
    }
}
