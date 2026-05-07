#ifndef RIVERS_QUEUE_H
#define RIVERS_QUEUE_H

/*
 * rivers_queue.h
 *
 * 这是从 rivers_osal 的 mem / queue 设计思路中独立出来的 standalone queue module。
 * 它不是原 OSAL 的替代实现，也不会依赖原 OSAL 的 task / timer / irq / queue /
 * mem / osal_config 等模块。
 *
 * 模块能力：
 * - rivers_mem：在一块静态内存池上实现动态分配和释放，不使用系统 malloc/free。
 * - rivers_queue：固定单元大小、固定单元数量的环形消息队列。
 *
 * 内存来源模型：
 * - rivers_mem_init(NULL, 0)：使用模块内部默认静态堆。
 * - rivers_mem_init(user_heap, heap_size)：使用用户提供的外部静态内存池。
 * - rivers_queue_create(length, item_size)：只从 rivers_mem 中分配队列控制块和数据区，
 *   不关心内存池实际来自内部默认堆还是用户外部堆。
 *
 * 推荐创建路径：
 *     rivers_mem_init(...) -> rivers_queue_create(...)
 *
 * 重要边界：
 * - 不依赖 HAL、CMSIS 或芯片厂商 SDK。
 * - 不提供 timeout、任务阻塞、等待链表、自动唤醒等 RTOS 语义。
 * - send / recv / from_isr 都是非阻塞立即尝试。
 * - 队列是固定单元 ring buffer，不是可变长消息队列。
 * - 适合在主循环和 ISR 之间传递固定大小消息。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 内部默认静态堆大小，单位字节。
 * @note 默认 4096U。rivers_mem_init(NULL, 0) 会使用 rivers_queue.c 内部静态数组作为内存池。
 * @note 内存池越大，占用 RAM 越多；队列控制块和队列数据区都会从这里分配。
 * @note 如果队列数量较多、队列长度较大或单元结构体较大，需要相应调大该值。
 */
#ifndef RIVERS_QUEUE_HEAP_SIZE
#define RIVERS_QUEUE_HEAP_SIZE 4096U
#endif

/**
 * @brief rivers_mem 分配对齐粒度，单位字节。
 * @note 默认 8U，适合大多数 32 位 MCU 常见数据对齐需求。
 * @note 移植到有更严格对齐要求的平台时可以调大，但会增加内部碎片。
 */
#ifndef RIVERS_QUEUE_MEM_ALIGN_SIZE
#define RIVERS_QUEUE_MEM_ALIGN_SIZE 8U
#endif

/**
 * @brief 是否启用 debug hook 上报。
 * @note 默认 0，不输出任何诊断信息。
 * @note 调试内存不足、非法参数、无效句柄等问题时，可以改成 1 并提供 RIVERS_QUEUE_DEBUG_HOOK。
 */
#ifndef RIVERS_QUEUE_ENABLE_DEBUG_HOOK
#define RIVERS_QUEUE_ENABLE_DEBUG_HOOK 0
#endif

/**
 * @brief 调试诊断钩子。
 * @param module 模块名字符串，例如 "mem" 或 "queue"。
 * @param message 诊断消息字符串。
 * @note 默认空实现。移植时可接入串口、RTT 或日志系统。
 */
#ifndef RIVERS_QUEUE_DEBUG_HOOK
#define RIVERS_QUEUE_DEBUG_HOOK(module, message) ((void)0)
#endif

/**
 * @brief rivers_queue 模块统一返回状态码。
 */
typedef enum {
    RIVERS_QUEUE_OK = 0,             /**< 操作成功。 */
    RIVERS_QUEUE_ERR_PARAM = -1,     /**< 参数非法，例如空指针、length 为 0 或 item_size 为 0。 */
    RIVERS_QUEUE_ERR_FULL = -2,      /**< 队列已满，send 不会覆盖旧数据。 */
    RIVERS_QUEUE_ERR_EMPTY = -3,     /**< 队列为空，recv 没有取到数据。 */
    RIVERS_QUEUE_ERR_NO_MEMORY = -4, /**< 静态堆空间不足。 */
    RIVERS_QUEUE_ERR_NOT_FOUND = -5, /**< 对象不存在或句柄无效，例如 delete 后继续使用。 */
    RIVERS_QUEUE_ERR_NOT_READY = -6  /**< 模块尚未初始化或状态不允许，当前接口预留该状态码。 */
} rivers_queue_status_t;

/**
 * @brief 不透明队列句柄。
 * @note 结构体定义在 rivers_queue.c 内部，用户只通过指针访问。
 */
typedef struct rivers_queue rivers_queue_t;

/**
 * @brief 初始化 rivers_mem 静态堆。
 * @param buffer 用户提供的外部内存池；传 NULL 时使用内部默认静态堆。
 * @param size buffer 的字节大小；buffer 为 NULL 或 size 为 0 时忽略并使用内部默认静态堆。
 * @return 无。
 * @note 这是静态内存池上的动态分配器，不是系统 malloc。
 * @note 重新初始化会清空原内存池状态；应在创建队列前调用。
 * @note queue 层的所有内存都来自 rivers_mem，因此内存池来源由本函数统一决定。
 * @note 注意：rivers_mem_init() 会重置内存池，也会清空活动队列链表。
 * @note 因此应在创建任何队列之前调用，不要在队列运行过程中重新初始化内存池。
 */
void rivers_mem_init(void *buffer, uint32_t size);

/**
 * @brief 从 rivers_mem 静态堆中分配一块内存。
 * @param size 请求的有效载荷大小，单位字节。
 * @return 成功返回可用内存指针，失败返回 NULL。
 * @note size 为 0 时返回 NULL。
 * @note 返回地址按 RIVERS_QUEUE_MEM_ALIGN_SIZE 对齐。
 * @note 这是静态内存池上的动态分配，不是系统 malloc。
 * @note 内部分配块由对齐后的 block header 和对齐后的 payload 组成。
 * @note 如果用户未调用 rivers_mem_init()，第一次分配会自动初始化内部默认静态堆。
 * @note 分配过程会进入临界区保护空闲块链表。
 */
void *rivers_mem_alloc(uint32_t size);

/**
 * @brief 释放由 rivers_mem_alloc() 返回的内存块。
 * @param ptr 待释放指针。
 * @return 无。
 * @note rivers_mem_free(NULL) 是安全空操作。
 * @note 会检查指针是否位于当前内存池范围内。
 * @note 会通过对齐后的 block header 反推出块头，并检查该 block 是否存在于当前块链表中。
 * @note 如果 ptr 不是 rivers_mem_alloc() 返回的有效指针，函数会安全返回并通过 debug hook 上报。
 * @note 释放后会尝试合并相邻空闲块，减少碎片。
 * @note 释放过程会进入临界区保护内存块链表。
 */
void rivers_mem_free(void *ptr);

/**
 * @brief 获取当前静态堆剩余可用空间。
 * @return 当前所有空闲块的总字节数，包含内部 block header 开销。
 * @note 适合观察总体剩余内存压力。
 * @note 该值是所有空闲块的总和，不代表一次最大可申请内存。
 * @note 如果想知道最大连续可申请 payload，请使用 rivers_mem_get_largest_free_block()。
 */
uint32_t rivers_mem_get_free_size(void);

/**
 * @brief 获取历史最低剩余内存。
 * @return 自最近一次 rivers_mem_init() 以来的最低 free size。
 * @note 可用于估算运行过程中的内存峰值压力。
 */
uint32_t rivers_mem_get_min_free_size(void);

/**
 * @brief 获取当前内存池中最大连续可申请 payload 大小。
 * @return 最大连续可申请有效载荷字节数；内存池未初始化或无可用空闲块时返回 0。
 * @note rivers_mem_get_free_size() 返回的是所有空闲块总和，不代表一次能申请这么大的连续内存。
 * @note 本接口更适合用于观察内存碎片情况，以及判断一次大块申请是否可能成功。
 */
uint32_t rivers_mem_get_largest_free_block(void);

/**
 * @brief 创建一个固定单元大小、固定单元数量的环形队列。
 * @param length 队列单元数量，也就是最多能缓存多少个 item。
 * @param item_size 每个队列单元的字节数，通常传 sizeof(消息结构体)。
 * @return 成功返回队列句柄，失败返回 NULL。
 * @note 创建完成后，length 和 item_size 固定不变。
 * @note 如果要改变容量或消息类型，应删除旧队列后重新创建。
 * @note 队列控制块和底层 storage 都从 rivers_mem 分配。
 * @note 内存池到底是内部默认堆还是用户外部堆，由 rivers_mem_init() 决定。
 * @note length == 0、item_size == 0 或 length * item_size 溢出时创建失败。
 */
rivers_queue_t *rivers_queue_create(uint32_t length, uint32_t item_size);

/**
 * @brief 删除队列。
 * @param q 队列句柄。
 * @return 无。
 * @note delete(NULL) 是安全空操作。
 * @note 会从活动队列链表中摘除 q，然后释放队列 storage 和控制块。
 * @note delete 后句柄立即失效，后续继续使用会被视为无效句柄。
 */
void rivers_queue_delete(rivers_queue_t *q);

/**
 * @brief 非阻塞发送一个固定大小队列单元。
 * @param q 队列句柄。
 * @param item 指向待发送数据的指针，至少 item_size 字节。
 * @return RIVERS_QUEUE_OK 表示成功；RIVERS_QUEUE_ERR_FULL 表示队列满；其他错误见 rivers_queue_status_t。
 * @note 内部使用 memcpy 拷贝 item_size 字节到 tail 位置。
 * @note 不等待、不 timeout、不覆盖旧数据。
 */
rivers_queue_status_t rivers_queue_send(rivers_queue_t *q, const void *item);

/**
 * @brief 非阻塞接收一个固定大小队列单元。
 * @param q 队列句柄。
 * @param item 接收缓冲区指针，至少 item_size 字节。
 * @return RIVERS_QUEUE_OK 表示成功；RIVERS_QUEUE_ERR_EMPTY 表示队列空；其他错误见 rivers_queue_status_t。
 * @note 内部使用 memcpy 从 head 位置拷贝 item_size 字节。
 * @note 不等待、不 timeout。
 */
rivers_queue_status_t rivers_queue_recv(rivers_queue_t *q, void *item);

/**
 * @brief ISR 中立即尝试发送一个队列单元。
 * @param q 队列句柄。
 * @param item 指向待发送数据的指针。
 * @return 行为和 rivers_queue_send() 一致。
 * @note 不等待、不阻塞、不 timeout。
 * @note 仍然会通过临界区保护 head / tail / count。
 */
rivers_queue_status_t rivers_queue_send_from_isr(rivers_queue_t *q, const void *item);

/**
 * @brief ISR 中立即尝试接收一个队列单元。
 * @param q 队列句柄。
 * @param item 接收缓冲区指针。
 * @return 行为和 rivers_queue_recv() 一致。
 * @note 不等待、不阻塞、不 timeout。
 * @note 仍然会通过临界区保护 head / tail / count。
 */
rivers_queue_status_t rivers_queue_recv_from_isr(rivers_queue_t *q, void *item);

/**
 * @brief 获取当前队列已有元素数量。
 * @param q 队列句柄。
 * @return 当前 count；q 无效时返回 0。
 */
uint32_t rivers_queue_get_count(const rivers_queue_t *q);

/**
 * @brief 获取当前队列剩余可写元素数量。
 * @param q 队列句柄。
 * @return length - count；q 无效时返回 0。
 */
uint32_t rivers_queue_get_free_count(const rivers_queue_t *q);

/**
 * @brief 获取队列容量。
 * @param q 队列句柄。
 * @return 队列单元数量；q 无效时返回 0。
 */
uint32_t rivers_queue_get_length(const rivers_queue_t *q);

/**
 * @brief 获取每个队列单元大小。
 * @param q 队列句柄。
 * @return 每个单元字节数；q 无效时返回 0。
 */
uint32_t rivers_queue_get_item_size(const rivers_queue_t *q);

/**
 * @brief 清空队列状态。
 * @param q 队列句柄。
 * @return RIVERS_QUEUE_OK 表示成功，其他值表示参数或句柄无效。
 * @note 会把 head / tail / count 清零，但不会清空 storage 中的旧内容。
 */
rivers_queue_status_t rivers_queue_reset(rivers_queue_t *q);

/*
 * 串口接收队列示例。
 *
 * 这个示例展示最常见的用法：UART 接收中断里快速把收到的字节入队，
 * 主循环里从队列取出字节并做协议解析。示例使用 HAL 风格函数名，
 * 但 rivers_queue 本身不依赖 HAL。
 *
 * #include "rivers_queue.h"
 *
 * typedef struct {
 *     uint8_t byte;
 * } uart_rx_msg_t;
 *
 * #define UART_RX_QUEUE_LEN 128
 *
 * static rivers_queue_t *s_uart_rx_queue;
 * static uint8_t s_uart_rx_byte;
 *
 * void app_init(void)
 * {
 *     rivers_mem_init(NULL, 0);
 *
 *     s_uart_rx_queue = rivers_queue_create(
 *         UART_RX_QUEUE_LEN,
 *         sizeof(uart_rx_msg_t)
 *     );
 *
 *     // 示例为 HAL 工程写法，rivers_queue 本身不依赖 HAL。
 *     // HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1);
 * }
 *
 * void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
 * {
 *     if (huart == &huart1) {
 *         uart_rx_msg_t msg;
 *         msg.byte = s_uart_rx_byte;
 *
 *         // 如果队列满，会返回 RIVERS_QUEUE_ERR_FULL，可按需求统计丢包或设置溢出标志。
 *         (void)rivers_queue_send_from_isr(s_uart_rx_queue, &msg);
 *
 *         // 继续启动下一次接收。
 *         // HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1);
 *     }
 * }
 *
 * void app_loop(void)
 * {
 *     uart_rx_msg_t msg;
 *
 *     while (rivers_queue_recv(s_uart_rx_queue, &msg) == RIVERS_QUEUE_OK) {
 *         // 在主循环中处理收到的字节，例如协议解析、帧组包、命令处理等。
 *         // uart_protocol_parse_byte(msg.byte);
 *     }
 * }
 *
 * 说明：
 * - 中断里只负责快速入队，不做复杂解析。
 * - 主循环里负责出队和协议解析。
 * - 如果串口速率很高，逐字节入队压力较大，可以改成 DMA + IDLE 中断，
 *   把帧长度、缓冲区指针或帧事件入队。
 * - 队列创建后，UART_RX_QUEUE_LEN 和 sizeof(uart_rx_msg_t) 固定不变。
 * - 如果不想使用内部默认静态堆，也可以自己提供一块内存池：
 *
 *   static uint8_t user_queue_heap[2048];
 *
 *   rivers_mem_init(user_queue_heap, sizeof(user_queue_heap));
 *   s_uart_rx_queue = rivers_queue_create(UART_RX_QUEUE_LEN, sizeof(uart_rx_msg_t));
 */

#ifdef __cplusplus
}
#endif

#endif /* RIVERS_QUEUE_H */

