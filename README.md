RTX Kernel

A small real-time operating system kernel I built with my group. It runs on an STM32 Nucleo-F401RE board (ARM Cortex-M4) and is written in C with a bit of inline ARM assembly.

What it does
Multitasking: create up to 16 tasks, each with its own stack. Tasks can yield, exit, and look up info about themselves.
EDF scheduling: the kernel always runs the task with the earliest deadline (earliest deadline first). Tasks without a set deadline default to 5 ms.
Preemption: SysTick fires every 1 ms and the kernel switches tasks if a more urgent one is ready.
Sleep and periodic tasks: osSleep and osPeriodYield let a task wait or run on a fixed period.
Memory allocator: a first-fit heap allocator with a free list sorted by address. Neighbouring free blocks get merged on free to reduce fragmentation.
System calls: user code calls the kernel through SVC instructions, and context switches happen in the PendSV handler.
Files
File	What's in it
Core/Src/k_task.c	Task management, the EDF scheduler, sleep and the tick handler
Core/Src/k_mem.c	Memory allocator
Core/Src/stm32f4xx_it.c	SVC, PendSV and SysTick interrupt handlers
Core/Inc/k_task.h, k_mem.h, common.h	Headers, TCB struct, constants
Core/Src/main.c	Test program

The rest (Drivers/, startup and linker files) is standard STM32CubeIDE project code.