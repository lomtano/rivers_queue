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
 * main.c 风格示例：UART RX / TX 两个固定单元环形队列。
 *
 * 这个示例使用 STM32 HAL 风格函数名，方便放进常见 CubeMX 工程理解；
 * rivers_queue 本身不依赖 HAL、CMSIS、串口驱动或任何 RTOS。
 *
 * 设计要点：
 * - 主示例使用内部默认静态堆：rivers_mem_init(NULL, 0)。
 * - RX 队列：UART 接收中断里逐字节接收，把 byte 封装成 uart_rx_msg_t 后快速入队。
 * - TX 队列：主循环把待发送字符串内容拷贝到 uart_tx_msg_t.text[] 后入队，
 *   再由主循环从 TX 队列取出并调用 HAL_UART_Transmit() 发送。
 * - 示例增加按行回显验证：主循环收到一行后，通过 TX 队列打印 "RX: <line>\r\n"。
 * - 队列满时不阻塞、不覆盖旧数据，直接丢弃并增加 drop 计数。
 * - 中断里只做快速入队和重新启动接收，不做复杂协议解析、printf 或字符串发送。
 * - 主循环负责 RX 出队后的组帧/解析，以及 TX 出队后的实际发送。
 * - 队列创建后，UART_RX_QUEUE_LEN、UART_TX_QUEUE_LEN、sizeof(uart_rx_msg_t)、
 *   sizeof(uart_tx_msg_t) 都固定不变；rivers_queue 是固定单元大小、固定数量的 ring buffer。
 * - send / recv / from_isr 都是非阻塞立即尝试；本模块不提供 timeout、阻塞等待、任务唤醒等 RTOS 语义。
 *
 * #include "rivers_queue.h"
 * #include <string.h>
 *
 * // RX 队列按字节缓存，因此一个队列单元只需要保存 1 个 byte。
 * // 中断回调里只把硬件收到的字节搬进队列，后续组帧/解析放到主循环。
 * typedef struct {
 *     uint8_t byte;
 * } uart_rx_msg_t;
 *
 * // TX 队列保存待发送字符串消息。
 * // len 记录本条消息有效长度，text[64] 保存字符串内容副本。
 * // 队列里保存内容副本，而不是字符串指针，可避免外部 buffer 被修改或失效。
 * typedef struct {
 *     uint16_t len;
 *     char text[64];
 * } uart_tx_msg_t;
 *
 * // UART_RX_QUEUE_LEN 表示 RX 队列最多缓存多少个接收字节。
 * // UART_TX_QUEUE_LEN 表示 TX 队列最多缓存多少条待发送字符串消息。
 * // UART_RX_LINE_BUF_SIZE 是主循环按行组帧缓存大小，不是 RX 队列容量。
 * #define UART_RX_QUEUE_LEN 128
 * #define UART_TX_QUEUE_LEN 8
 * #define UART_RX_LINE_BUF_SIZE 64
 *
 * extern UART_HandleTypeDef huart1;
 *
 * // RX / TX 两个队列分别用于中断到主循环的接收通道、主循环到串口发送的打印通道。
 * static rivers_queue_t *s_uart_rx_queue;
 * static rivers_queue_t *s_uart_tx_queue;
 *
 * // HAL_UART_Receive_IT() 使用的单字节接收缓存；调试时可观察它是否变化。
 * static uint8_t s_uart_rx_byte;
 *
 * // 队列满时不阻塞等待，只增加 drop 计数，方便观察是否处理不及时或队列长度不足。
 * static volatile uint32_t s_uart_rx_drop_count;
 * static volatile uint32_t s_uart_tx_drop_count;
 *
 * // 供调试器 Watch 窗口观察当前队列积压数量。
 * static volatile uint32_t s_uart_rx_count_debug;
 * static volatile uint32_t s_uart_tx_count_debug;
 *
 * // 主循环中使用的按行组帧缓存：普通字符先进入 line_buf，收到 '\n' 后认为一行结束。
 * static char s_uart_rx_line_buf[UART_RX_LINE_BUF_SIZE];
 * static uint16_t s_uart_rx_line_len;
 *
 * // 把字符串封装成 uart_tx_msg_t 并放入 TX 队列，由 uart_tx_poll() 统一发送。
 * // 这里使用 memcpy() 把字符串内容拷贝到 msg.text[]，避免只传指针带来的生命周期问题。
 * // 如果字符串超过 text[] 容量，会按当前消息单元容量截断。
 * // TX 队列满时只增加 s_uart_tx_drop_count，不阻塞等待。
 * static void uart_queue_put_string(const char *text)
 * {
 *     uart_tx_msg_t msg;
 *     size_t len;
 *
 *     if ((text == NULL) || (s_uart_tx_queue == NULL)) {
 *         return;
 *     }
 *
 *     len = strlen(text);
 *     if (len > sizeof(msg.text)) {
 *         len = sizeof(msg.text);
 *     }
 *
 *     msg.len = (uint16_t)len;
 *     memcpy(msg.text, text, len);
 *
 *     if (rivers_queue_send(s_uart_tx_queue, &msg) != RIVERS_QUEUE_OK) {
 *         s_uart_tx_drop_count++;
 *     }
 * }
 *
 * // 主循环中的单字节处理函数，用于演示最简单的按行回显。
 * // '\r' 被忽略，用于兼容串口助手常见的 "\r\n" 行尾。
 * // 收到 '\n' 表示一行结束，只有此时才通过 TX 队列回显 "RX: xxx\r\n"。
 * // 普通字符先进入 s_uart_rx_line_buf；行缓存满时清空当前行并提示 overflow。
 * static void uart_rx_handle_byte(uint8_t byte)
 * {
 *     if (byte == '\r') {
 *         return;
 *     }
 *
 *     if (byte == '\n') {
 *         if (s_uart_rx_line_len > 0U) {
 *             s_uart_rx_line_buf[s_uart_rx_line_len] = '\0';
 *             uart_queue_put_string("RX: ");
 *             uart_queue_put_string(s_uart_rx_line_buf);
 *             uart_queue_put_string("\r\n");
 *             s_uart_rx_line_len = 0U;
 *         }
 *         return;
 *     }
 *
 *     if (s_uart_rx_line_len < (UART_RX_LINE_BUF_SIZE - 1U)) {
 *         s_uart_rx_line_buf[s_uart_rx_line_len] = (char)byte;
 *         s_uart_rx_line_len++;
 *     } else {
 *         s_uart_rx_line_len = 0U;
 *         uart_queue_put_string("RX line overflow\r\n");
 *     }
 * }
 *
 * // 运行在主循环中：不断从 RX 队列取出字节，再交给 uart_rx_handle_byte() 组帧/解析。
 * // 协议解析、命令处理、打印触发都应放在这里，不放在串口中断回调里。
 * // 前后更新 s_uart_rx_count_debug，便于调试器观察 RX 队列积压变化。
 * static void uart_rx_poll(void)
 * {
 *     uart_rx_msg_t msg;
 *
 *     s_uart_rx_count_debug = rivers_queue_get_count(s_uart_rx_queue);
 *
 *     while (rivers_queue_recv(s_uart_rx_queue, &msg) == RIVERS_QUEUE_OK) {
 *         // 在主循环中组帧/解析；中断里只负责快速入队。
 *         uart_rx_handle_byte(msg.byte);
 *     }
 *
 *     s_uart_rx_count_debug = rivers_queue_get_count(s_uart_rx_queue);
 * }
 *
 * // 运行在主循环中：从 TX 队列取出字符串消息并调用 HAL_UART_Transmit() 发送。
 * // HAL_UART_Transmit() 是阻塞式发送，本示例用于验证链路；正式项目大量发送时可换成 IT/DMA 发送。
 * // 前后更新 s_uart_tx_count_debug，便于调试器观察 TX 队列积压变化。
 * static void uart_tx_poll(void)
 * {
 *     uart_tx_msg_t msg;
 *
 *     s_uart_tx_count_debug = rivers_queue_get_count(s_uart_tx_queue);
 *
 *     while (rivers_queue_recv(s_uart_tx_queue, &msg) == RIVERS_QUEUE_OK) {
 *         (void)HAL_UART_Transmit(
 *             &huart1,
 *             (uint8_t *)msg.text,
 *             msg.len,
 *             100U
 *         );
 *     }
 *
 *     s_uart_tx_count_debug = rivers_queue_get_count(s_uart_tx_queue);
 * }
 *
 * int main(void)
 * {
 *     // HAL 工程常规初始化：初始化 HAL、系统时钟和 USART1 外设。
 *     HAL_Init();
 *     SystemClock_Config();
 *     MX_USART1_UART_Init();
 *
 *     // 使用 rivers_queue.c 内部默认静态堆，不使用用户自定义 user_queue_heap。
 *     rivers_mem_init(NULL, 0);
 *
 *     // 创建 RX 队列：固定缓存 UART_RX_QUEUE_LEN 个 uart_rx_msg_t 单元。
 *     s_uart_rx_queue = rivers_queue_create(
 *         UART_RX_QUEUE_LEN,
 *         sizeof(uart_rx_msg_t)
 *     );
 *
 *     // 创建 TX 队列：固定缓存 UART_TX_QUEUE_LEN 个 uart_tx_msg_t 单元。
 *     s_uart_tx_queue = rivers_queue_create(
 *         UART_TX_QUEUE_LEN,
 *         sizeof(uart_tx_msg_t)
 *     );
 *
 *     if ((s_uart_rx_queue == NULL) || (s_uart_tx_queue == NULL)) {
 *         while (1) {
 *             // 队列创建失败，通常是 RIVERS_QUEUE_HEAP_SIZE 不足或参数配置错误。
 *         }
 *     }
 *
 *     // 启动第一次 1 字节中断接收；后续每次接收完成后在回调里重新启动。
 *     (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
 *
 *     // 启动提示字符串先进入 TX 队列，随后由 uart_tx_poll() 统一发送。
 *     uart_queue_put_string("rivers_queue start\r\n");
 *     uart_queue_put_string("send a line and press enter\r\n");
 *
 *     while (1) {
 *         // 周期性轮询 RX/TX 队列；调用越及时，队列积压和丢弃风险越低。
 *         uart_rx_poll();
 *         uart_tx_poll();
 *
 *         // 其他主循环业务可以放在这里；不要长时间阻塞，否则 RX/TX 队列处理会变慢。
 *     }
 * }
 *
 * // HAL 串口接收完成回调。
 * // 这里只把收到的 1 个字节放入 RX 队列，并重新开启下一次接收。
 * // 不要在这里做协议解析、printf、HAL_UART_Transmit() 或其他耗时业务。
 * void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
 * {
 *     if (huart == &huart1) {
 *         uart_rx_msg_t msg;
 *
 *         msg.byte = s_uart_rx_byte;
 *
 *         if (rivers_queue_send_from_isr(s_uart_rx_queue, &msg) != RIVERS_QUEUE_OK) {
 *             s_uart_rx_drop_count++;
 *         }
 *
 *         (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
 *     }
 * }
 *
 * 验证方法：
 * - 串口助手发送 hello rivers，并设置发送结尾为 "\r\n" 或 "\n"。
 * - 正常应收到回显：RX: hello rivers。
 * - 如果没有发送换行符，数据会暂存在 s_uart_rx_line_buf 中，不会立即打印。
 * - 可以在调试器 Watch 中观察：s_uart_rx_byte、s_uart_rx_drop_count、
 *   s_uart_tx_drop_count、s_uart_rx_count_debug、s_uart_tx_count_debug、
 *   s_uart_rx_line_len、s_uart_rx_line_buf。
 * - 如果 s_uart_rx_byte 变化，说明中断接收到数据。
 * - 如果 s_uart_rx_drop_count 增加，说明 RX 队列处理不及时或长度不足。
 * - 如果 s_uart_tx_drop_count 增加，说明 TX 队列积压或发送不及时。
 * - 如果 s_uart_rx_line_buf 有内容但没有回显，通常是没有收到 '\n'。
 * - 如果启动字符串能打印但回显不工作，优先检查 HAL_UART_Receive_IT 是否成功启动，以及回调是否进入。
 *
 * 高速串口建议：
 * - 当前示例是逐字节中断接收并逐字节入队，适合低速串口或普通功能验证。
 * - 如果串口速率很高，逐字节中断和逐字节入队压力较大。
 * - 正式项目可以改成 DMA + IDLE 中断，把帧长度、缓冲区指针或帧事件放入队列。
 * - 这时队列传递的就不是单字节，而是“帧事件”或“缓冲区描述符”。
 *
 * 如果不想使用内部默认静态堆，也可以自己提供一块内存池。
 * 这只是补充方式，不是上面主示例的主线：
 *
 * static uint8_t user_queue_heap[2048];
 *
 * rivers_mem_init(user_queue_heap, sizeof(user_queue_heap));
 */
#ifdef __cplusplus
}
#endif

#endif /* RIVERS_QUEUE_H */


