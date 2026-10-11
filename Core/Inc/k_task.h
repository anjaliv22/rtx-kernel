/*
 * k_task.h  –  handles the task management API for RTX kernel
 */

#ifndef INC_K_TASK_H_
#define INC_K_TASK_H_

#include "common.h"

/* default deadline for tasks created with osCreateTask */
#define DEFAULT_DEADLINE_MS  5

/* Task Control Block */
typedef struct task_control_block {
    void (*ptask)(void* args); /* entry address */
    U32 stack_high; //top of stack
    task_t tid; //task ID
    U8 state; //4 states DORMANT / READY / RUNNING / SLEEPING 
    U16 stack_size; //stack size in bytes -> 8

    U32 psp; //saved PSP -> pendsv
    void* args; //argument pointer passed to ptask 

    U32 deadline; // period / deadline in ms 
    U32 time_remaining; // ms remaining in current period (for EDF + preemption) 
    U32 sleep_remaining; // ms remaining in sleep when state == SLEEPING) 
    U8 stack_dynamic; // 1 if stack was dynamically allocated  

    U32 period_start; // absolute g_tick_count value when current period began 
} TCB;


/* ---- Public API (SVC-backed) ---- */

void osKernelInit(void);
int osCreateTask(TCB* task);
int osKernelStart(void);
void osYield(void);
int osTaskInfo(task_t TID, TCB* task_copy);
task_t osGetTID(void);
int osTaskExit(void);
void osSleep(int timeInMs);
void osPeriodYield(void);
int osSetDeadline(int deadline, task_t TID);
int    osCreateDeadlineTask(int deadline, TCB* task);


/* kernel side implementations -> should be called from svc*/
void k_osKernelInit(void);
int k_osCreateTask(TCB* task);
int k_osKernelStart(void);
void k_osYield(void);
int k_osTaskInfo(task_t TID, TCB* task_copy);
task_t k_osGetTID(void);
int k_osTaskExit(void);

void k_osSleep(int timeInMs);
// bool k_osSleep(int timeInMs);
void k_osPeriodYield(void);
int k_osSetDeadline(int deadline, task_t TID);
int k_osCreateDeadlineTask(int deadline, TCB* task);

void k_tick(void); //call from systick 

/* PendSV helpers */
void k_pendsv_save_psp(U32 psp);
U32 k_pendsv_schedule_next(void);

task_t k_get_current_tid(void);
int k_is_kernel_running(void);

#endif /* INC_K_TASK_H_ */
