/*
 * rivers_queue.c
 *
 * 这是从 rivers_osal 的 mem / queue 设计思路中独立出来的 standalone queue module。
 * 它不是原 OSAL 的替代实现，也不 include 或依赖 HAL、CMSIS、原 OSAL 头文件。
 *
 * 本文件包含两部分：
 * - rivers_mem：静态内存池上的动态分配器，不使用系统 malloc/free。
 * - rivers_queue：固定单元大小、固定单元数量的环形消息队列。
 *
 * 内存来源由 rivers_mem_init() 统一决定。rivers_queue_create() 始终从 rivers_mem
 * 分配队列控制块和底层 storage，不再接收用户直接传入的队列 storage buffer。
 *
 * 本模块不实现 timeout、任务阻塞、等待链表、自动唤醒等 RTOS 语义。
 * send/recv/from_isr 都是立即尝试，适合主循环和 ISR 之间传递固定大小消息。
 */

#include "rivers_queue.h"
#include <string.h>

#if RIVERS_QUEUE_ENABLE_DEBUG_HOOK
#define RIVERS_QUEUE_REPORT(module, message) RIVERS_QUEUE_DEBUG_HOOK((module), (message))
#else
#define RIVERS_QUEUE_REPORT(module, message) ((void)0)
#endif

/*
 * 极简 PRIMASK 临界区。
 *
 * 默认实现面向 Cortex-M，不依赖 CMSIS。进入临界区时保存 PRIMASK 并关中断，
 * 退出时只在进入前原本开中断的情况下重新开中断。
 *
 * 如果编译器不支持内联汇编，下面提供空实现，但真实工程应通过宏覆盖：
 * RIVERS_QUEUE_CRITICAL_ENTER() / RIVERS_QUEUE_CRITICAL_EXIT(state)。
 */
#ifndef RIVERS_QUEUE_CRITICAL_ENTER
#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief 进入默认 PRIMASK 临界区。
 * @return 进入前的 PRIMASK 状态。
 */
static uint32_t rivers_queue_default_critical_enter(void) {
    uint32_t primask;

    __asm volatile (
        "mrs %0, primask\n"
        "cpsid i\n"
        : "=r" (primask)
        :
        : "memory");
    return primask;
}

/**
 * @brief 退出默认 PRIMASK 临界区。
 * @param state rivers_queue_default_critical_enter() 返回的 PRIMASK 状态。
 * @return 无。
 */
static void rivers_queue_default_critical_exit(uint32_t state) {
    if (state == 0U) {
        __asm volatile ("cpsie i" ::: "memory");
    }
}
#define RIVERS_QUEUE_CRITICAL_ENTER() rivers_queue_default_critical_enter()
#define RIVERS_QUEUE_CRITICAL_EXIT(state) rivers_queue_default_critical_exit((state))
#elif defined(__ICCARM__)
/**
 * @brief IAR 编译器下进入默认 PRIMASK 临界区。
 * @return 进入前的 PRIMASK 状态。
 */
static uint32_t rivers_queue_default_critical_enter(void) {
    uint32_t primask;

    __asm volatile ("MRS %0, PRIMASK" : "=r" (primask));
    __asm volatile ("CPSID i");
    return primask;
}

/**
 * @brief IAR 编译器下退出默认 PRIMASK 临界区。
 * @param state 进入前的 PRIMASK 状态。
 * @return 无。
 */
static void rivers_queue_default_critical_exit(uint32_t state) {
    if (state == 0U) {
        __asm volatile ("CPSIE i");
    }
}
#define RIVERS_QUEUE_CRITICAL_ENTER() rivers_queue_default_critical_enter()
#define RIVERS_QUEUE_CRITICAL_EXIT(state) rivers_queue_default_critical_exit((state))
#else
/**
 * @brief 无内联汇编支持时的空临界区占位实现。
 * @return 固定返回 0。
 * @note 真实工程中应替换为有效的关中断/加锁实现。
 */
static uint32_t rivers_queue_default_critical_enter(void) {
    return 0U;
}

/**
 * @brief 无内联汇编支持时的空临界区退出占位实现。
 * @param state 占位参数。
 * @return 无。
 */
static void rivers_queue_default_critical_exit(uint32_t state) {
    (void)state;
}
#define RIVERS_QUEUE_CRITICAL_ENTER() rivers_queue_default_critical_enter()
#define RIVERS_QUEUE_CRITICAL_EXIT(state) rivers_queue_default_critical_exit((state))
#endif
#endif

/** @brief 静态堆块头。 */
typedef struct rivers_mem_block {
    uint32_t size;                  /* 当前块总大小，包含块头。 */
    bool free;                      /* true 表示空闲，false 表示已分配。 */
    struct rivers_mem_block *next;  /* 按地址顺序串起来的下一个块。 */
} rivers_mem_block_t;

struct rivers_queue {
    uint8_t *storage;
    uint32_t head;
    uint32_t tail;
    uint32_t length;
    uint32_t item_size;
    uint32_t count;
    struct rivers_queue *next;
};

typedef union {
    void *align_ptr;
    uint64_t align_u64;
    uint8_t bytes[RIVERS_QUEUE_HEAP_SIZE];
} rivers_default_heap_t;

static rivers_default_heap_t s_default_heap;
static uint8_t *s_heap_start = NULL;
static uint32_t s_heap_size = 0U;
static rivers_mem_block_t *s_heap_head = NULL;
static bool s_mem_ready = false;
static uint32_t s_min_free_size = 0U;
static rivers_queue_t *s_queue_list = NULL;

/**
 * @brief 向上按 RIVERS_QUEUE_MEM_ALIGN_SIZE 对齐数值。
 * @param value 原始数值。
 * @return 对齐后的数值。
 */
static uint32_t rivers_align_up_u32(uint32_t value) {
    uint32_t align = RIVERS_QUEUE_MEM_ALIGN_SIZE;
    uint32_t rem;

    if (align == 0U) {
        align = 1U;
    }

    rem = value % align;
    return (rem == 0U) ? value : (value + (align - rem));
}

/**
 * @brief 获取对齐后的内存块头大小。
 * @return 按 RIVERS_QUEUE_MEM_ALIGN_SIZE 向上对齐后的 block header 字节数。
 * @note 用户 payload 紧跟在该对齐块头之后，不能直接使用 sizeof(rivers_mem_block_t) 做偏移。
 */
static uint32_t rivers_mem_header_size(void) {
    return rivers_align_up_u32((uint32_t)sizeof(rivers_mem_block_t));
}

/**
 * @brief 向下按 RIVERS_QUEUE_MEM_ALIGN_SIZE 对齐数值。
 * @param value 原始数值。
 * @return 对齐后的数值。
 */
static uint32_t rivers_align_down_u32(uint32_t value) {
    uint32_t align = RIVERS_QUEUE_MEM_ALIGN_SIZE;

    if (align == 0U) {
        align = 1U;
    }

    return value - (value % align);
}

/**
 * @brief 向上对齐指针地址。
 * @param ptr 原始指针。
 * @return 对齐后的地址。
 */
static uintptr_t rivers_align_up_ptr(uintptr_t ptr) {
    uintptr_t align = (uintptr_t)RIVERS_QUEUE_MEM_ALIGN_SIZE;
    uintptr_t rem;

    if (align == 0U) {
        align = 1U;
    }

    rem = ptr % align;
    return (rem == 0U) ? ptr : (ptr + (align - rem));
}

/**
 * @brief 计算当前空闲块总大小。
 * @return 当前所有 free block 的 size 总和。
 * @note 调用方应已经进入临界区。
 */
static uint32_t rivers_mem_free_size_locked(void) {
    uint32_t total = 0U;
    rivers_mem_block_t *block = s_heap_head;

    while (block != NULL) {
        if (block->free) {
            total += block->size;
        }
        block = block->next;
    }

    return total;
}

/**
 * @brief 更新历史最低剩余内存。
 * @return 无。
 * @note 调用方应已经进入临界区。
 */
static void rivers_mem_update_min_free_locked(void) {
    uint32_t free_size = rivers_mem_free_size_locked();

    if (free_size < s_min_free_size) {
        s_min_free_size = free_size;
    }
}

/**
 * @brief 确保内存池已经初始化。
 * @return 无。
 * @note 如果用户未显式初始化，则自动使用内部默认静态堆。
 */
static void rivers_mem_ensure_init(void) {
    if (!s_mem_ready) {
        rivers_mem_init(NULL, 0U);
    }
}

void rivers_mem_init(void *buffer, uint32_t size) {
    uint32_t state;
    uintptr_t raw_start;
    uintptr_t aligned_start;
    uint32_t adjust;
    uint32_t aligned_size;
    uint32_t header_size;

    if ((buffer == NULL) || (size == 0U)) {
        buffer = s_default_heap.bytes;
        size = (uint32_t)sizeof(s_default_heap.bytes);
    }

    raw_start = (uintptr_t)buffer;
    aligned_start = rivers_align_up_ptr(raw_start);
    adjust = (uint32_t)(aligned_start - raw_start);

    if (size <= adjust) {
        aligned_size = 0U;
    } else {
        aligned_size = rivers_align_down_u32(size - adjust);
    }
    header_size = rivers_mem_header_size();

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    if (aligned_size < (header_size + RIVERS_QUEUE_MEM_ALIGN_SIZE)) {
        s_heap_start = NULL;
        s_heap_size = 0U;
        s_heap_head = NULL;
        s_mem_ready = false;
        s_min_free_size = 0U;
        s_queue_list = NULL;
        RIVERS_QUEUE_CRITICAL_EXIT(state);
        RIVERS_QUEUE_REPORT("mem", "heap buffer is too small");
        return;
    }

    s_heap_start = (uint8_t *)aligned_start;
    s_heap_size = aligned_size;
    s_heap_head = (rivers_mem_block_t *)s_heap_start;
    s_heap_head->size = s_heap_size;
    s_heap_head->free = true;
    s_heap_head->next = NULL;
    s_mem_ready = true;
    s_min_free_size = s_heap_size;
    s_queue_list = NULL;
    RIVERS_QUEUE_CRITICAL_EXIT(state);
}

/**
 * @brief 尝试把空闲块拆分成已分配块和剩余空闲块。
 * @param block 当前被分配的空闲块。
 * @param request_size 请求总大小，包含块头。
 * @return 无。
 * @note 调用方应已经进入临界区。
 */
static void rivers_mem_split_block_locked(rivers_mem_block_t *block, uint32_t request_size) {
    uint32_t header_size = rivers_mem_header_size();
    uint32_t remain = block->size - request_size;

    if (remain > (header_size + RIVERS_QUEUE_MEM_ALIGN_SIZE)) {
        rivers_mem_block_t *next = (rivers_mem_block_t *)((uint8_t *)block + request_size);
        next->size = remain;
        next->free = true;
        next->next = block->next;
        block->size = request_size;
        block->next = next;
    }

    block->free = false;
}

void *rivers_mem_alloc(uint32_t size) {
    uint32_t state;
    uint32_t header_size;
    uint32_t payload_size;
    uint32_t request_size;
    rivers_mem_block_t *block;

    if (size == 0U) {
        return NULL;
    }

    rivers_mem_ensure_init();
    if (!s_mem_ready) {
        return NULL;
    }

    header_size = rivers_mem_header_size();
    payload_size = rivers_align_up_u32(size);
    if (payload_size > (0xFFFFFFFFUL - header_size)) {
        RIVERS_QUEUE_REPORT("mem", "allocation size overflow");
        return NULL;
    }
    request_size = rivers_align_up_u32(header_size + payload_size);

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    block = s_heap_head;
    while (block != NULL) {
        if (block->free && (block->size >= request_size)) {
            rivers_mem_split_block_locked(block, request_size);
            rivers_mem_update_min_free_locked();
            RIVERS_QUEUE_CRITICAL_EXIT(state);
            /*
             * block 起始地址已按 RIVERS_QUEUE_MEM_ALIGN_SIZE 对齐，header_size 也经过同样对齐，
             * 因此返回的 payload 地址同样满足 RIVERS_QUEUE_MEM_ALIGN_SIZE 对齐。
             */
            return (uint8_t *)block + header_size;
        }
        block = block->next;
    }
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    RIVERS_QUEUE_REPORT("mem", "heap out of memory");
    return NULL;
}

/**
 * @brief 判断指针是否位于当前内存池范围内。
 * @param ptr 待检查指针。
 * @return true 表示在范围内。
 */
static bool rivers_mem_ptr_in_heap(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;

    return (s_heap_start != NULL) && (p >= s_heap_start) && (p < (s_heap_start + s_heap_size));
}

/**
 * @brief 合并当前块后面连续的空闲块。
 * @param block 当前块。
 * @return 无。
 * @note 调用方应已经进入临界区。
 */
static void rivers_mem_coalesce_next_locked(rivers_mem_block_t *block) {
    while ((block != NULL) && (block->next != NULL) && block->next->free) {
        uint8_t *block_end = (uint8_t *)block + block->size;
        if (block_end != (uint8_t *)block->next) {
            break;
        }
        block->size += block->next->size;
        block->next = block->next->next;
    }
}

void rivers_mem_free(void *ptr) {
    uint32_t state;
    uint32_t header_size;
    uint8_t *p;
    rivers_mem_block_t *block;
    rivers_mem_block_t *prev;
    rivers_mem_block_t *current;

    if (ptr == NULL) {
        return;
    }

    rivers_mem_ensure_init();
    header_size = rivers_mem_header_size();
    p = (uint8_t *)ptr;

    if ((s_heap_start == NULL) || (s_heap_size == 0U) || !s_mem_ready) {
        RIVERS_QUEUE_REPORT("mem", "heap is not ready");
        return;
    }

    if (!rivers_mem_ptr_in_heap(ptr)) {
        RIVERS_QUEUE_REPORT("mem", "free pointer is outside heap");
        return;
    }

    if (p < (s_heap_start + header_size)) {
        RIVERS_QUEUE_REPORT("mem", "free pointer is before first payload");
        return;
    }

    block = (rivers_mem_block_t *)(p - header_size);

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    prev = NULL;
    current = s_heap_head;
    while ((current != NULL) && (current != block)) {
        prev = current;
        current = current->next;
    }

    if ((current == NULL) || current->free) {
        RIVERS_QUEUE_CRITICAL_EXIT(state);
        RIVERS_QUEUE_REPORT("mem", "free pointer is invalid or duplicated");
        return;
    }

    current->free = true;
    rivers_mem_coalesce_next_locked(current);
    if ((prev != NULL) && prev->free) {
        rivers_mem_coalesce_next_locked(prev);
    }
    RIVERS_QUEUE_CRITICAL_EXIT(state);
}

uint32_t rivers_mem_get_free_size(void) {
    uint32_t state;
    uint32_t total;

    rivers_mem_ensure_init();
    if (!s_mem_ready) {
        return 0U;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    total = rivers_mem_free_size_locked();
    RIVERS_QUEUE_CRITICAL_EXIT(state);
    return total;
}

uint32_t rivers_mem_get_min_free_size(void) {
    uint32_t state;
    uint32_t min_free;

    rivers_mem_ensure_init();
    if (!s_mem_ready) {
        return 0U;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    min_free = s_min_free_size;
    RIVERS_QUEUE_CRITICAL_EXIT(state);
    return min_free;
}

uint32_t rivers_mem_get_largest_free_block(void) {
    uint32_t state;
    uint32_t header_size;
    uint32_t largest = 0U;
    rivers_mem_block_t *block;

    rivers_mem_ensure_init();
    if (!s_mem_ready) {
        return 0U;
    }

    header_size = rivers_mem_header_size();

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    block = s_heap_head;
    while (block != NULL) {
        if (block->free && (block->size > header_size)) {
            uint32_t payload = block->size - header_size;
            if (payload > largest) {
                largest = payload;
            }
        }
        block = block->next;
    }
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return largest;
}

/**
 * @brief 检查 length * item_size 是否有效并计算数据区总大小。
 * @param length 队列单元数量。
 * @param item_size 单元大小。
 * @param total_size 输出总字节数。
 * @return true 表示合法。
 */
static bool rivers_queue_storage_size(uint32_t length, uint32_t item_size, uint32_t *total_size) {
    uint64_t bytes;

    if ((length == 0U) || (item_size == 0U) || (total_size == NULL)) {
        return false;
    }

    bytes = (uint64_t)length * (uint64_t)item_size;
    if (bytes > 0xFFFFFFFFULL) {
        return false;
    }

    *total_size = (uint32_t)bytes;
    return true;
}

/**
 * @brief 把队列控制块加入活动队列链表。
 * @param q 队列控制块。
 * @return 无。
 * @note 调用方应已经进入临界区。
 */
static void rivers_queue_link_locked(rivers_queue_t *q) {
    q->next = s_queue_list;
    s_queue_list = q;
}

/**
 * @brief 判断队列句柄是否仍在活动链表中。
 * @param q 队列句柄。
 * @return true 表示句柄有效。
 * @note 调用方应已经进入临界区。
 */
static bool rivers_queue_is_active_locked(const rivers_queue_t *q) {
    const rivers_queue_t *current = s_queue_list;

    while (current != NULL) {
        if (current == q) {
            return true;
        }
        current = current->next;
    }

    return false;
}

/**
 * @brief 从活动队列链表中摘除队列。
 * @param q 队列句柄。
 * @return true 表示成功摘除。
 * @note 调用方应已经进入临界区。
 */
static bool rivers_queue_unlink_locked(rivers_queue_t *q) {
    rivers_queue_t *prev = NULL;
    rivers_queue_t *current = s_queue_list;

    while (current != NULL) {
        if (current == q) {
            if (prev == NULL) {
                s_queue_list = current->next;
            } else {
                prev->next = current->next;
            }
            current->next = NULL;
            return true;
        }
        prev = current;
        current = current->next;
    }

    return false;
}

/**
 * @brief 初始化队列控制块公共字段并加入活动队列链表。
 * @param storage 已经由 rivers_mem_alloc() 分配好的底层数据区。
 * @param length 队列容量。
 * @param item_size 单元大小。
 * @return 成功返回队列句柄，失败返回 NULL。
 * @note 如果控制块分配失败，会释放已经分配成功的 storage，避免内存泄漏。
 */
static rivers_queue_t *rivers_queue_create_internal(uint8_t *storage, uint32_t length, uint32_t item_size) {
    uint32_t state;
    rivers_queue_t *q;

    if (storage == NULL) {
        return NULL;
    }

    q = (rivers_queue_t *)rivers_mem_alloc((uint32_t)sizeof(rivers_queue_t));
    if (q == NULL) {
        rivers_mem_free(storage);
        RIVERS_QUEUE_REPORT("queue", "queue control block allocation failed");
        return NULL;
    }

    q->storage = storage;
    q->head = 0U;
    q->tail = 0U;
    q->length = length;
    q->item_size = item_size;
    q->count = 0U;
    q->next = NULL;

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    rivers_queue_link_locked(q);
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return q;
}

rivers_queue_t *rivers_queue_create(uint32_t length, uint32_t item_size) {
    uint32_t total_size;
    uint8_t *storage;

    if (!rivers_queue_storage_size(length, item_size, &total_size)) {
        RIVERS_QUEUE_REPORT("queue", "invalid queue create parameters");
        return NULL;
    }

    storage = (uint8_t *)rivers_mem_alloc(total_size);
    if (storage == NULL) {
        RIVERS_QUEUE_REPORT("queue", "queue storage allocation failed");
        return NULL;
    }

    return rivers_queue_create_internal(storage, length, item_size);
}

void rivers_queue_delete(rivers_queue_t *q) {
    uint32_t state;
    bool active;
    uint8_t *storage;

    if (q == NULL) {
        return;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    active = rivers_queue_unlink_locked(q);
    storage = active ? q->storage : NULL;
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    if (!active) {
        RIVERS_QUEUE_REPORT("queue", "delete inactive queue handle");
        return;
    }

    rivers_mem_free(storage);
    rivers_mem_free(q);
}

/**
 * @brief 在不进入临界区的前提下压入一个队列单元。
 * @param q 队列句柄。
 * @param item 源数据。
 * @return 状态码。
 * @note 调用方必须已经确认 q 有效并进入临界区。
 */
static rivers_queue_status_t rivers_queue_enqueue_locked(rivers_queue_t *q, const void *item) {
    if (q->count >= q->length) {
        return RIVERS_QUEUE_ERR_FULL;
    }

    memcpy(q->storage + (q->tail * q->item_size), item, q->item_size);
    q->tail = (q->tail + 1U) % q->length;
    q->count++;
    return RIVERS_QUEUE_OK;
}

/**
 * @brief 在不进入临界区的前提下弹出一个队列单元。
 * @param q 队列句柄。
 * @param item 目标缓冲区。
 * @return 状态码。
 * @note 调用方必须已经确认 q 有效并进入临界区。
 */
static rivers_queue_status_t rivers_queue_dequeue_locked(rivers_queue_t *q, void *item) {
    if (q->count == 0U) {
        return RIVERS_QUEUE_ERR_EMPTY;
    }

    memcpy(item, q->storage + (q->head * q->item_size), q->item_size);
    q->head = (q->head + 1U) % q->length;
    q->count--;
    return RIVERS_QUEUE_OK;
}

rivers_queue_status_t rivers_queue_send(rivers_queue_t *q, const void *item) {
    uint32_t state;
    rivers_queue_status_t status;

    if ((q == NULL) || (item == NULL)) {
        return RIVERS_QUEUE_ERR_PARAM;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    if (!rivers_queue_is_active_locked(q)) {
        RIVERS_QUEUE_CRITICAL_EXIT(state);
        RIVERS_QUEUE_REPORT("queue", "send inactive queue handle");
        return RIVERS_QUEUE_ERR_NOT_FOUND;
    }
    status = rivers_queue_enqueue_locked(q, item);
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return status;
}

rivers_queue_status_t rivers_queue_recv(rivers_queue_t *q, void *item) {
    uint32_t state;
    rivers_queue_status_t status;

    if ((q == NULL) || (item == NULL)) {
        return RIVERS_QUEUE_ERR_PARAM;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    if (!rivers_queue_is_active_locked(q)) {
        RIVERS_QUEUE_CRITICAL_EXIT(state);
        RIVERS_QUEUE_REPORT("queue", "recv inactive queue handle");
        return RIVERS_QUEUE_ERR_NOT_FOUND;
    }
    status = rivers_queue_dequeue_locked(q, item);
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return status;
}

rivers_queue_status_t rivers_queue_send_from_isr(rivers_queue_t *q, const void *item) {
    return rivers_queue_send(q, item);
}

rivers_queue_status_t rivers_queue_recv_from_isr(rivers_queue_t *q, void *item) {
    return rivers_queue_recv(q, item);
}

uint32_t rivers_queue_get_count(const rivers_queue_t *q) {
    uint32_t state;
    uint32_t count;

    if (q == NULL) {
        return 0U;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    if (!rivers_queue_is_active_locked(q)) {
        count = 0U;
    } else {
        count = q->count;
    }
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return count;
}

uint32_t rivers_queue_get_free_count(const rivers_queue_t *q) {
    uint32_t state;
    uint32_t free_count;

    if (q == NULL) {
        return 0U;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    if (!rivers_queue_is_active_locked(q)) {
        free_count = 0U;
    } else {
        free_count = q->length - q->count;
    }
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return free_count;
}

uint32_t rivers_queue_get_length(const rivers_queue_t *q) {
    uint32_t state;
    uint32_t length;

    if (q == NULL) {
        return 0U;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    length = rivers_queue_is_active_locked(q) ? q->length : 0U;
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return length;
}

uint32_t rivers_queue_get_item_size(const rivers_queue_t *q) {
    uint32_t state;
    uint32_t item_size;

    if (q == NULL) {
        return 0U;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    item_size = rivers_queue_is_active_locked(q) ? q->item_size : 0U;
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return item_size;
}

rivers_queue_status_t rivers_queue_reset(rivers_queue_t *q) {
    uint32_t state;

    if (q == NULL) {
        return RIVERS_QUEUE_ERR_PARAM;
    }

    state = RIVERS_QUEUE_CRITICAL_ENTER();
    if (!rivers_queue_is_active_locked(q)) {
        RIVERS_QUEUE_CRITICAL_EXIT(state);
        RIVERS_QUEUE_REPORT("queue", "reset inactive queue handle");
        return RIVERS_QUEUE_ERR_NOT_FOUND;
    }

    q->head = 0U;
    q->tail = 0U;
    q->count = 0U;
    RIVERS_QUEUE_CRITICAL_EXIT(state);

    return RIVERS_QUEUE_OK;
}

