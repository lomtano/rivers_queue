# rivers_queue

`rivers_queue` 是一个从 `rivers_osal` 的 `mem / queue` 设计思路中独立出来的 **standalone queue module**。

它适合用于小型裸机工程，提供两类能力：

- `rivers_mem`：基于静态内存池的动态分配器
- `rivers_queue`：固定单元大小、固定单元数量的环形消息队列

模块只包含两个文件：

```text
rivers_queue.h
rivers_queue.c
```

本模块不依赖 HAL、CMSIS，也不依赖原 `rivers_osal` 的 `task / timer / irq / queue / mem / osal_config` 等模块；不使用系统 `malloc/free`；不提供 timeout、任务阻塞、等待链表、自动唤醒等 RTOS 语义。

---

## 1. 功能特性

```text
rivers_queue
│
├── rivers_mem：静态内存池动态分配
│   ├── rivers_mem_init()
│   ├── rivers_mem_alloc()
│   ├── rivers_mem_free()
│   ├── rivers_mem_get_free_size()
│   ├── rivers_mem_get_min_free_size()
│   └── rivers_mem_get_largest_free_block()
│
└── rivers_queue：固定单元环形队列
    ├── rivers_queue_create()
    ├── rivers_queue_delete()
    ├── rivers_queue_send()
    ├── rivers_queue_recv()
    ├── rivers_queue_send_from_isr()
    ├── rivers_queue_recv_from_isr()
    ├── rivers_queue_get_count()
    ├── rivers_queue_get_free_count()
    ├── rivers_queue_get_length()
    ├── rivers_queue_get_item_size()
    └── rivers_queue_reset()
```

---

## 2. 设计定位

`rivers_queue` 的核心定位是：

> 在裸机工程中，用一套轻量、可移植、无系统 malloc 依赖的方式，实现固定大小消息的缓存与传递。

它适合：

- 主循环与中断之间传递消息
- UART 接收字节缓存
- 按键事件缓存
- 传感器采样事件缓存
- 状态机之间传递固定大小事件
- 小型裸机工程中的非阻塞消息队列

它不适合：

- 可变长消息队列
- RTOS 任务阻塞等待
- timeout 阻塞收发
- 自动任务唤醒
- 大块数据频繁整块 memcpy
- 高吞吐大帧数据直接入队

如果需要传递大帧数据，更推荐队列里存放“指针、索引、长度、事件类型”等描述信息，而不是把整帧数据 memcpy 进队列。

---

## 3. 内存来源模型

本模块把“内存池来源”和“队列创建”分开。

### 3.1 使用内部默认静态堆

```c
rivers_mem_init(NULL, 0);
```

这种方式使用 `rivers_queue.c` 内部定义的默认静态数组作为内存池。

默认大小由宏控制：

```c
#define RIVERS_QUEUE_HEAP_SIZE 4096U
```

### 3.2 使用用户提供的外部静态内存池

```c
static uint8_t user_queue_heap[2048];

rivers_mem_init(user_queue_heap, sizeof(user_queue_heap));
```

这种方式适合用户希望自己控制 RAM 布局，例如把队列内存放到指定 SRAM、指定 section 或一块专门预留的内存区域中。

### 3.3 队列统一创建

无论使用内部默认堆，还是用户外部堆，队列都统一通过：

```c
rivers_queue_t *q = rivers_queue_create(length, item_size);
```

创建。

也就是说：

```text
rivers_mem_init(...) -> rivers_queue_create(...)
```

`queue` 层不关心内存池来自哪里，它只从 `rivers_mem` 中分配队列控制块和队列数据区。

---

## 4. 基本使用流程

### 4.1 添加文件

把下面两个文件加入工程：

```text
rivers_queue.h
rivers_queue.c
```

在需要使用队列的文件中包含头文件：

```c
#include "rivers_queue.h"
```

### 4.2 初始化内存池

```c
rivers_mem_init(NULL, 0);
```

或者使用用户自定义内存池：

```c
static uint8_t user_queue_heap[2048];

rivers_mem_init(user_queue_heap, sizeof(user_queue_heap));
```

> 注意：`rivers_mem_init()` 会重置内存池，同时清空活动队列链表。  
> 因此应在创建任何队列之前调用，不要在队列运行过程中重新初始化内存池。

### 4.3 创建队列

```c
typedef struct {
    uint16_t id;
    uint8_t data[8];
} app_msg_t;

rivers_queue_t *queue = rivers_queue_create(16, sizeof(app_msg_t));
```

这表示创建一个队列：

```text
队列容量：16 个单元
每个单元大小：sizeof(app_msg_t)
```

创建完成后，`length` 和 `item_size` 固定不变。  
如果需要改变容量或消息类型，应删除旧队列后重新创建。

---

## 5. API 说明

## 5.1 rivers_mem

### `rivers_mem_init`

```c
void rivers_mem_init(void *buffer, uint32_t size);
```

初始化静态内存池。

参数：

- `buffer`：用户提供的内存池；传 `NULL` 使用内部默认静态堆
- `size`：用户内存池大小；传 `0` 使用内部默认静态堆

注意：

- 应在创建队列之前调用。
- 重新初始化会清空原内存池状态。
- 运行过程中不要重新初始化，否则已有队列句柄会失效。

---

### `rivers_mem_alloc`

```c
void *rivers_mem_alloc(uint32_t size);
```

从静态内存池中分配一块内存。

特点：

- 不使用系统 `malloc`
- 返回地址按 `RIVERS_QUEUE_MEM_ALIGN_SIZE` 对齐
- 内部分配块由对齐后的 block header 和对齐后的 payload 组成
- 如果用户未调用 `rivers_mem_init()`，第一次分配会自动初始化默认静态堆

---

### `rivers_mem_free`

```c
void rivers_mem_free(void *ptr);
```

释放由 `rivers_mem_alloc()` 返回的内存块。

特点：

- `rivers_mem_free(NULL)` 是安全空操作
- 会检查指针是否位于当前内存池范围内
- 会检查反推出的 block 是否存在于当前块链表中
- 释放后会尝试合并相邻空闲块，减少碎片

---

### `rivers_mem_get_free_size`

```c
uint32_t rivers_mem_get_free_size(void);
```

获取当前所有空闲块的总大小。

注意：

- 返回值包含内部 block header 开销
- 适合观察总体剩余内存压力
- 不代表一次最大可申请内存

---

### `rivers_mem_get_min_free_size`

```c
uint32_t rivers_mem_get_min_free_size(void);
```

获取自最近一次 `rivers_mem_init()` 以来的历史最低剩余内存。

它可用于估算运行过程中的内存峰值压力。

---

### `rivers_mem_get_largest_free_block`

```c
uint32_t rivers_mem_get_largest_free_block(void);
```

获取当前内存池中最大连续可申请 payload 大小。

它比 `rivers_mem_get_free_size()` 更适合观察内存碎片情况。

例如：

```text
free_size = 1024
largest_free_block = 256
```

这表示当前总空闲空间还有 1024 字节，但最大连续可申请空间只有 256 字节。

---

## 5.2 rivers_queue

### `rivers_queue_create`

```c
rivers_queue_t *rivers_queue_create(uint32_t length, uint32_t item_size);
```

创建一个固定单元大小、固定单元数量的环形队列。

参数：

- `length`：队列单元数量
- `item_size`：每个队列单元的字节数

返回值：

- 成功：返回队列句柄
- 失败：返回 `NULL`

注意：

- 队列控制块和底层数据区都从 `rivers_mem` 分配
- `length == 0` 时创建失败
- `item_size == 0` 时创建失败
- `length * item_size` 溢出时创建失败
- 创建完成后，容量和单元大小固定不变

---

### `rivers_queue_delete`

```c
void rivers_queue_delete(rivers_queue_t *q);
```

删除队列。

特点：

- `delete(NULL)` 是安全空操作
- 会释放队列数据区和队列控制块
- 删除后句柄立即失效

---

### `rivers_queue_send`

```c
rivers_queue_status_t rivers_queue_send(rivers_queue_t *q, const void *item);
```

非阻塞发送一个队列单元。

行为：

- 队列未满：复制 `item_size` 字节到 `tail` 位置
- 队列已满：返回 `RIVERS_QUEUE_ERR_FULL`
- 不等待、不阻塞、不覆盖旧数据

---

### `rivers_queue_recv`

```c
rivers_queue_status_t rivers_queue_recv(rivers_queue_t *q, void *item);
```

非阻塞接收一个队列单元。

行为：

- 队列非空：从 `head` 位置复制 `item_size` 字节到用户缓冲区
- 队列为空：返回 `RIVERS_QUEUE_ERR_EMPTY`
- 不等待、不阻塞

---

### `rivers_queue_send_from_isr`

```c
rivers_queue_status_t rivers_queue_send_from_isr(rivers_queue_t *q, const void *item);
```

中断中立即尝试发送一个队列单元。

它不等待、不阻塞、不 timeout，行为与 `rivers_queue_send()` 一致。

---

### `rivers_queue_recv_from_isr`

```c
rivers_queue_status_t rivers_queue_recv_from_isr(rivers_queue_t *q, void *item);
```

中断中立即尝试接收一个队列单元。

它不等待、不阻塞、不 timeout，行为与 `rivers_queue_recv()` 一致。

---

### `rivers_queue_get_count`

```c
uint32_t rivers_queue_get_count(const rivers_queue_t *q);
```

获取当前队列已有元素数量。

---

### `rivers_queue_get_free_count`

```c
uint32_t rivers_queue_get_free_count(const rivers_queue_t *q);
```

获取当前队列剩余可写元素数量。

---

### `rivers_queue_get_length`

```c
uint32_t rivers_queue_get_length(const rivers_queue_t *q);
```

获取队列容量。

---

### `rivers_queue_get_item_size`

```c
uint32_t rivers_queue_get_item_size(const rivers_queue_t *q);
```

获取每个队列单元大小。

---

### `rivers_queue_reset`

```c
rivers_queue_status_t rivers_queue_reset(rivers_queue_t *q);
```

清空队列状态。

它会把：

```text
head = 0
tail = 0
count = 0
```

但不会清空底层 `storage` 中的旧内容。

---

## 6. 运行原理

### 6.1 环形队列模型

队列内部使用：

```text
storage
head
tail
length
item_size
count
```

其中：

- `storage`：底层数据区
- `head`：下一个读取位置
- `tail`：下一个写入位置
- `length`：队列容量
- `item_size`：每个单元大小
- `count`：当前已有元素数量

发送时：

```text
memcpy(storage + tail * item_size, item, item_size)
tail = (tail + 1) % length
count++
```

接收时：

```text
memcpy(item, storage + head * item_size, item_size)
head = (head + 1) % length
count--
```

队列空：

```text
count == 0
```

队列满：

```text
count == length
```

---

### 6.2 临界区保护

本模块默认使用 Cortex-M `PRIMASK` 实现临界区：

```text
进入临界区：
    保存 PRIMASK
    cpsid i 关闭可屏蔽中断

退出临界区：
    如果进入前 PRIMASK == 0
        cpsie i 恢复中断
```

它不是简单粗暴地“关中断后一定开中断”，而是保存并恢复进入前的中断状态，避免破坏外层已经关中断的逻辑。

临界区主要保护：

- 内存块链表
- 活动队列链表
- 队列 `head / tail / count`

注意：`PRIMASK` 只能屏蔽普通可屏蔽中断，不能屏蔽 NMI、HardFault 等异常。

---

## 7. 串口接收示例

下面示例展示最常见的用法：UART 接收中断中快速入队，主循环中出队并解析。

示例使用 HAL 风格函数名，但 `rivers_queue` 本身不依赖 HAL。

```c
#include "rivers_queue.h"

typedef struct {
    uint8_t byte;
} uart_rx_msg_t;

#define UART_RX_QUEUE_LEN 128

static rivers_queue_t *s_uart_rx_queue;
static uint8_t s_uart_rx_byte;

void app_init(void)
{
    rivers_mem_init(NULL, 0);

    s_uart_rx_queue = rivers_queue_create(
        UART_RX_QUEUE_LEN,
        sizeof(uart_rx_msg_t)
    );

    /*
     * 示例为 HAL 工程写法，rivers_queue 本身不依赖 HAL。
     */
    // HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        uart_rx_msg_t msg;
        msg.byte = s_uart_rx_byte;

        /*
         * 如果队列满，会返回 RIVERS_QUEUE_ERR_FULL。
         * 用户可按需求统计丢包或设置溢出标志。
         */
        (void)rivers_queue_send_from_isr(s_uart_rx_queue, &msg);

        /*
         * 继续启动下一次接收。
         */
        // HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1);
    }
}

void app_loop(void)
{
    uart_rx_msg_t msg;

    while (rivers_queue_recv(s_uart_rx_queue, &msg) == RIVERS_QUEUE_OK) {
        /*
         * 在主循环中处理收到的字节。
         * 例如：协议解析、帧组包、命令处理等。
         */
        // uart_protocol_parse_byte(msg.byte);
    }
}
```

### 高速串口建议

如果串口速率很高，逐字节入队可能压力较大。

更推荐：

```text
DMA + IDLE 中断
```

然后把下面这些“帧事件信息”入队：

```text
缓冲区索引
帧长度
接收完成标志
时间戳
```

而不是把每个字节都放入队列。

---

## 8. 状态码

```c
typedef enum {
    RIVERS_QUEUE_OK = 0,
    RIVERS_QUEUE_ERR_PARAM = -1,
    RIVERS_QUEUE_ERR_FULL = -2,
    RIVERS_QUEUE_ERR_EMPTY = -3,
    RIVERS_QUEUE_ERR_NO_MEMORY = -4,
    RIVERS_QUEUE_ERR_NOT_FOUND = -5,
    RIVERS_QUEUE_ERR_NOT_READY = -6
} rivers_queue_status_t;
```

| 状态码 | 含义 |
|---|---|
| `RIVERS_QUEUE_OK` | 操作成功 |
| `RIVERS_QUEUE_ERR_PARAM` | 参数非法，例如空指针、`length == 0` 或 `item_size == 0` |
| `RIVERS_QUEUE_ERR_FULL` | 队列已满 |
| `RIVERS_QUEUE_ERR_EMPTY` | 队列为空 |
| `RIVERS_QUEUE_ERR_NO_MEMORY` | 静态堆空间不足 |
| `RIVERS_QUEUE_ERR_NOT_FOUND` | 对象不存在或句柄无效 |
| `RIVERS_QUEUE_ERR_NOT_READY` | 模块尚未初始化或状态不允许，当前预留 |

---

## 9. 配置宏

### 9.1 默认静态堆大小

```c
#ifndef RIVERS_QUEUE_HEAP_SIZE
#define RIVERS_QUEUE_HEAP_SIZE 4096U
#endif
```

控制内部默认静态堆大小。

如果队列数量多、队列长度大、单元结构体大，需要适当调大该值。

---

### 9.2 内存对齐粒度

```c
#ifndef RIVERS_QUEUE_MEM_ALIGN_SIZE
#define RIVERS_QUEUE_MEM_ALIGN_SIZE 8U
#endif
```

控制 `rivers_mem_alloc()` 返回地址的对齐粒度。

默认 8 字节，适合大多数 32 位 MCU 常见数据对齐需求。

---

### 9.3 Debug Hook

```c
#ifndef RIVERS_QUEUE_ENABLE_DEBUG_HOOK
#define RIVERS_QUEUE_ENABLE_DEBUG_HOOK 0
#endif

#ifndef RIVERS_QUEUE_DEBUG_HOOK
#define RIVERS_QUEUE_DEBUG_HOOK(module, message) ((void)0)
#endif
```

默认关闭调试输出。

如果需要调试内存不足、非法参数、无效句柄等问题，可以启用 debug hook，并把它接到串口、RTT 或日志系统。

---

## 10. 注意事项

1. `rivers_mem_init()` 应在创建任何队列之前调用。
2. 不要在队列运行过程中重新调用 `rivers_mem_init()`，否则已有队列句柄会失效。
3. `rivers_queue_create()` 创建完成后，队列容量和单元大小固定不变。
4. `send / recv` 都是非阻塞立即尝试。
5. 队列满时不会覆盖旧数据，而是返回 `RIVERS_QUEUE_ERR_FULL`。
6. 队列空时 `recv` 返回 `RIVERS_QUEUE_ERR_EMPTY`。
7. 队列内部使用 `memcpy` 拷贝 `item_size` 字节。
8. 不建议把特别大的数据结构作为队列单元；大数据建议传指针、索引或描述符。
9. `send_from_isr / recv_from_isr` 也是立即尝试，不提供等待语义。
10. 本模块不是 RTOS Queue，不提供任务阻塞、等待链表和自动唤醒。
11. 本模块不使用系统 `malloc/free`，只使用静态内存池。
12. 如果编译器不支持内联汇编，默认临界区会退化为空实现，真实工程中应替换为有效的关中断或加锁实现。
