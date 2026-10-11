/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "stm32f4xx_it.h"
#include <stdint.h>
#include "common.h"
#include "k_task.h"
#include "k_mem.h"

/* -----------------------------------------------------------------------
 * SVC handler  –  dispatches to kernel implementations
 * --------------------------------------------------------------------- */
void SVC_Handler_Main(uint32_t *svc_args)
{
    uint8_t svc_no = ((uint8_t*)svc_args[6])[-2];

    switch (svc_no) {
        /* ── Deliverable 1 ── */
        case SVC_KINIT:
            k_osKernelInit();
            break;
        case SVC_CREATE:
            svc_args[0] = (uint32_t)k_osCreateTask((TCB*)svc_args[0]);
            break;
        case SVC_START:
            svc_args[0] = (uint32_t)k_osKernelStart();
            break;
        case SVC_YIELD:
            k_osYield();
            break;
        case SVC_TASKINFO:
            svc_args[0] = (uint32_t)k_osTaskInfo((task_t)svc_args[0], (TCB*)svc_args[1]);
            break;
        case SVC_GETTID:
            svc_args[0] = (uint32_t)k_osGetTID();
            break;
        case SVC_EXIT:
            svc_args[0] = (uint32_t)k_osTaskExit();
            break;

        /* ── Deliverable 2 ── */
        case SVC_MEM_INIT:
            svc_args[0] = (uint32_t)k_k_mem_init();
            break;
        case SVC_MEM_ALLOC:
            svc_args[0] = (uint32_t)k_k_mem_alloc((size_t)svc_args[0]);
            break;
        case SVC_MEM_DEALLOC:
            svc_args[0] = (uint32_t)k_k_mem_dealloc((void*)svc_args[0]);
            break;
        case SVC_MEM_EXTFRAG:
            svc_args[0] = (uint32_t)k_k_mem_count_extfrag((size_t)svc_args[0]);
            break;

        /* ── Deliverable 3 ── */
        case SVC_SLEEP:
            k_osSleep((int)svc_args[0]);
            break;
        case SVC_PERIOD_YIELD:
            k_osPeriodYield();
            break;
        case SVC_SET_DEADLINE:
            svc_args[0] = (uint32_t)k_osSetDeadline((int)svc_args[0], (task_t)svc_args[1]);
            break;
        case SVC_CREATE_DEADLINE:
            svc_args[0] = (uint32_t)k_osCreateDeadlineTask((int)svc_args[0], (TCB*)svc_args[1]);
            break;

        default:
            svc_args[0] = (uint32_t)RTX_ERR;
            break;
    }
}

/* -----------------------------------------------------------------------
 * Cortex-M4 exception handlers
 * --------------------------------------------------------------------- */

void NMI_Handler(void)           { while (1) {} }
void HardFault_Handler(void)     { while (1) {} }
void MemManage_Handler(void)     { while (1) {} }
void BusFault_Handler(void)      { while (1) {} }
void UsageFault_Handler(void)    { while (1) {} }

__attribute__((naked)) void SVC_Handler(void)
{
    __asm volatile(
        "tst lr, #4           \n"
        "ite eq               \n"
        "mrseq r0, msp        \n"
        "mrsne r0, psp        \n"
        "b SVC_Handler_Main   \n"
    );
}

void DebugMon_Handler(void) {}

__attribute__((naked)) void PendSV_Handler(void)
{
    __asm volatile(
        "mrs r0, psp                \n"
        "cmp r0, #0                 \n"
        "beq skip_save              \n"
        "stmdb r0!, {r4-r11}        \n"
        "bl k_pendsv_save_psp       \n"
        "skip_save:                 \n"
        "bl  k_pendsv_schedule_next \n"
        "ldmia r0!, {r4-r11}        \n"
        "msr psp, r0                \n"
        "ldr lr, =0xFFFFFFFD        \n"
        "bx  lr                     \n"
    );
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    /* Call our tick handler every 1 ms for deadline/sleep countdown */
    k_tick();
}
