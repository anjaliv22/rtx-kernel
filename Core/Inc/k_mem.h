/*
 * k_mem.h
 */

#ifndef INC_K_MEM_H_
#define INC_K_MEM_H_

#include "common.h"
#include <stddef.h>   /* size_t */

// Public (SVC-backed) API
int k_mem_init(void);
void *k_mem_alloc(size_t size);
int k_mem_dealloc(void *ptr);
int k_mem_count_extfrag(size_t size);

// Kernel-side implementations (called from SVC handler)
int k_k_mem_init(void);
void *k_k_mem_alloc(size_t size);
int k_k_mem_dealloc(void *ptr);
int k_k_mem_count_extfrag(size_t size);

//New SVC numbers -> being added after SVC_EXIT = 6 in common.h)
#define SVC_MEM_INIT 7
#define SVC_MEM_ALLOC 8
// #define SVC_MEM_ALLOC 9
#define SVC_MEM_DEALLOC 9
#define SVC_MEM_EXTFRAG 10


#endif
