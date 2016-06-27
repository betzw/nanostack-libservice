/*
 * Copyright (c) 2014-2015 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdint.h>
#include <string.h>
#include "nsdynmemLIB.h"
#include "platform/arm_hal_interrupt.h"
#include <stdlib.h>

void (*heap_failure_callback)(heap_fail_t);

#ifndef STANDARD_MALLOC
static int *heap_main = 0;
static int *heap_main_end = 0;
static uint16_t heap_size = 0;

typedef enum mem_stat_update_t {
    DEV_HEAP_ALLOC_OK,
    DEV_HEAP_ALLOC_FAIL,
    DEV_HEAP_FREE,
} mem_stat_update_t;


static mem_stat_t *mem_stat_info_ptr = 0;


static void heap_failure(heap_fail_t reason)
{
    if (heap_failure_callback) {
        heap_failure_callback(reason);
    }
}

#endif

void ns_dyn_mem_init(uint8_t *heap, uint16_t h_size, void (*passed_fptr)(heap_fail_t), mem_stat_t *info_ptr)
{
#ifndef STANDARD_MALLOC
    int *ptr;
    int temp_int;
    /* Do memory alignment */
    temp_int = ((uintptr_t)heap % sizeof(int));
    if (temp_int) {
        heap += (sizeof(int) - temp_int);
        h_size -= (sizeof(int) - temp_int);
    }

    /* Make correction for total length also */
    temp_int = (h_size % sizeof(int));
    if (temp_int) {
        h_size -= (sizeof(int) - temp_int);
    }
    heap_main = (int *)heap; // SET Heap Pointer
    heap_size = h_size; //Set Heap Size
    temp_int = (h_size / sizeof(int));
    temp_int -= 2;
    ptr = heap_main;
    *ptr = -(temp_int);
    ptr += (temp_int + 1);
    *ptr = -(temp_int);
    heap_main_end = ptr;
    //RESET Memory by Hea Len
    if (info_ptr) {
        mem_stat_info_ptr = info_ptr;
        memset(mem_stat_info_ptr, 0, sizeof(mem_stat_t));
        mem_stat_info_ptr->heap_sector_size = heap_size;
    }
#endif
    heap_failure_callback = passed_fptr;
}

const mem_stat_t *ns_dyn_mem_get_mem_stat(void)
{
#ifndef STANDARD_MALLOC
    return mem_stat_info_ptr;
#else
    return NULL;
#endif
}

#ifndef STANDARD_MALLOC
void dev_stat_update(mem_stat_update_t type, int16_t size)
{
    if (mem_stat_info_ptr) {
        switch (type) {
            case DEV_HEAP_ALLOC_OK:
                mem_stat_info_ptr->heap_sector_alloc_cnt++;
                mem_stat_info_ptr->heap_sector_allocated_bytes += size;
                if (mem_stat_info_ptr->heap_sector_allocated_bytes_max < mem_stat_info_ptr->heap_sector_allocated_bytes) {
                    mem_stat_info_ptr->heap_sector_allocated_bytes_max = mem_stat_info_ptr->heap_sector_allocated_bytes;
                }
                mem_stat_info_ptr->heap_alloc_total_bytes += size;
                break;
            case DEV_HEAP_ALLOC_FAIL:
                mem_stat_info_ptr->heap_alloc_fail_cnt++;
                break;
            case DEV_HEAP_FREE:
                mem_stat_info_ptr->heap_sector_alloc_cnt--;
                mem_stat_info_ptr->heap_sector_allocated_bytes -= size;
                break;
        }
    }
}

static int convert_allocation_size(int16_t requested_bytes)
{
    if (heap_main == 0) {
        heap_failure(NS_DYN_MEM_HEAP_SECTOR_UNITIALIZED);
    } else if (requested_bytes < 1) {
        heap_failure(NS_DYN_MEM_ALLOCATE_SIZE_NOT_VALID);
    } else if (requested_bytes > (heap_size - 2 * sizeof(int)) ) {
        heap_failure(NS_DYN_MEM_ALLOCATE_SIZE_NOT_VALID);
    }
    return (requested_bytes + sizeof(int) - 1) / sizeof(int);
}

// Checks that block length indicators are valid
// Block has format: Size of data area [1 word] | data area [abs(size) words]| Size of data area [1 word]
// If Size is negative it means area is unallocated
// For direction, use 1 for direction up and -1 for down
static int8_t ns_block_validate(int *block_start, int direction)
{
    int8_t ret_val = -1;
    int *end = block_start;
    int size_start = *end;
    if (direction > 0) {
        end += (1 + abs(size_start));
    } else {
        end -= (1 + abs(size_start));
    }

    if (size_start != 0 && size_start == *end) {
        ret_val = 0;
    }
    return ret_val;
}
#endif

// For direction, use 1 for direction up and -1 for down
static void *ns_dyn_mem_internal_alloc(const int16_t alloc_size, int direction)
{
#ifndef STANDARD_MALLOC
    void *retval = 0;
    int data_size = convert_allocation_size(alloc_size);
    if (data_size) {
        int h_size = (heap_size / sizeof(int));
        int *ptr = direction > 0 ? heap_main : heap_main_end;
        int moved = 0;
        bool update_start = true;
        platform_enter_critical();
        while (moved < h_size) {
            if (ns_block_validate(ptr, direction) == 0) {
                int block_data_size = *ptr;

                if (block_data_size < 0) {
                    //This is a free block so stop updating pointer
                    update_start = false;

                    block_data_size = -block_data_size;
                    if (block_data_size >= data_size) {
                        /*found block*/
                        if (block_data_size > (data_size + 4)) {
                            //There is enough room for a new hole so create it first
                            int *hole_ptr;
                            if (direction > 0) {
                                hole_ptr = ptr + data_size + 2;
                            } else {
                                hole_ptr = ptr - (data_size + 2);
                            }
                            int hole_size = block_data_size - data_size - 2;
                            *hole_ptr = -(hole_size);
                            if (direction > 0) {
                                hole_ptr += (hole_size + 1);
                            } else {
                                hole_ptr -= (hole_size + 1);
                            }
                            *hole_ptr = -(hole_size);
                        }else{
                            data_size = block_data_size;
                        }

                        *ptr = data_size;
                        if (direction > 0) {
                            ptr++;
                            retval = ptr;
                            ptr += data_size;
                        } else {
                            ptr -= data_size;
                            retval = ptr;
                            ptr--;
                        }
                        *ptr = data_size;
                        break;
                    }
                }

                moved += (block_data_size + 2);
                if (direction > 0) {
                    ptr += block_data_size + 2;
                } else {
                    ptr -= (block_data_size + 2);
                }
            } else {
                heap_failure(NS_DYN_MEM_HEAP_SECTOR_CORRUPTED);
                retval = 0;
                break;
            }
        }


        if (mem_stat_info_ptr) {
            if (retval) {
                //Update Allocate OK
                dev_stat_update(DEV_HEAP_ALLOC_OK, (data_size + 2) * sizeof(int));

            } else {
                //Update Allocate Fail, second parameter is not used for stats
                dev_stat_update(DEV_HEAP_ALLOC_FAIL, 0);
            }
        }
        platform_exit_critical();
    }
    return retval;
#else
    void *retval = 0;
    if (alloc_size) {
        platform_enter_critical();
        retval = malloc(alloc_size);
        platform_exit_critical();
    }
    return retval;
#endif
}

void *ns_dyn_mem_alloc(int16_t alloc_size)
{
    return ns_dyn_mem_internal_alloc(alloc_size, -1);
}

void *ns_dyn_mem_temporary_alloc(int16_t alloc_size)
{
    return ns_dyn_mem_internal_alloc(alloc_size, 1);
}

#ifndef STANDARD_MALLOC
static void ns_free_and_merge_with_adjacent_blocks(int *cur_block, int data_size)
{
    // Theory of operation: Block is always in form | Len | Data | Len |
    // So we need to check length of previous (if current not heap start)
    // and next (if current not heap end) blocks. Negative length means
    // free memory so we can merge freed block with those.

    int *start = cur_block;
    int *end = cur_block + data_size + 1;
    *cur_block = -data_size;
    *end = -data_size;
    int merged_data_size = data_size;

    if (cur_block != heap_main) {
        cur_block--;
        if (*cur_block < 0) {
            merged_data_size += (2 - *cur_block);
            start -= (2 - *cur_block);
        }
        cur_block++;
    }

    if (end != heap_main_end) {
        end++;
        if (*end < 0) {
            merged_data_size += (2 - *end);
            end += (1 - *end);
        }else{
            end--;
        }
    }

    *start = -merged_data_size;
    *end = -merged_data_size;
}
#endif

#ifdef STANDARD_MALLOC
#ifdef USE_IAR
#pragma optimize=none
#endif
#endif
void ns_dyn_mem_free(void *block)
{
#ifndef STANDARD_MALLOC
    int *ptr = block;
    int size;

    if (!block) {
        return;
    }

    if (!heap_main) {
        heap_failure(NS_DYN_MEM_HEAP_SECTOR_UNITIALIZED);
        return;
    }

    platform_enter_critical();
    ptr --;
    //Read Current Size
    size = *ptr;
    if (size < 0) {
        heap_failure(NS_DYN_MEM_DOUBLE_FREE);
    } else if (ptr < heap_main || ptr >= heap_main_end) {
        heap_failure(NS_DYN_MEM_POINTER_NOT_VALID);
    } else if ((ptr + size) >= heap_main_end) {
        heap_failure(NS_DYN_MEM_POINTER_NOT_VALID);
    } else {
        if (ns_block_validate(ptr, 1) != 0) {
            heap_failure(NS_DYN_MEM_HEAP_SECTOR_CORRUPTED);
        } else {
            ns_free_and_merge_with_adjacent_blocks(ptr, size);
            if (mem_stat_info_ptr) {
                //Update Free Counter
                dev_stat_update(DEV_HEAP_FREE, (size + 2) * sizeof(int));
            }
        }
    }
    platform_exit_critical();
#else
    platform_enter_critical();
    free(block);
    platform_exit_critical();
#endif
}

#ifndef STANDARD_MALLOC
#ifdef DEV_STAT

int16_t ns_dyn_mem_longest_free_block(void)
{

    int *ptr;
    int size, h_size;
    int scanned = 0;
    int16_t longest_block = 0;
    if (heap_main) {
        h_size = heap_size / 4;
        ptr = heap_main;
        platform_enter_critical();
        while (scanned < h_size) {
            size = *ptr;
            if (size < 0) {
                size = -size;
                if (size > longest_block) {
                    longest_block = size;
                }
            }
            if (size == 0) {
                heap_failure(NS_DYN_MEM_HEAP_SECTOR_CORRUPTED);
                platform_exit_critical();
                return 0;
            }
            ptr += size + 2;
            scanned += (size + 2);
        }
        platform_exit_critical();
    }
    return longest_block;
}
#endif
#endif
