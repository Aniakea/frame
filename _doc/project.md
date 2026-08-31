# ESP32-S3 嵌入式插件化框架 - 技术架构修订草案 v4.1

版本: 4.1-draft
状态: 架构修订草案；仅批准实施可行性原型，未批准完整组件编码
硬件目标: ESP32-S3 (双核 Xtensa LX7, 240MHz), 16MB PSRAM, 512KB 内部 SRAM
显示目标: 300x400 RLCD/SPI，面板支持 60Hz
软件基线: ESP-IDF v6.0.2（固定 release tag 与工具链）, IDF FreeRTOS 双核, C++26 (核心) + C ABI (插件边界)

批准条件:

· 本文档中的动态 ELF、PSRAM/IRAM 执行、60fps、SD 故障恢复和断电日志均为待验证目标，而非已证实能力
· 只有附录 D 定义的五类 PoC 均提供真实板级证据后，文档状态才能改为“批准完整组件编码”
· 任一核心 PoC 失败时必须重新进行架构评审，不自动退化为静态插件或脚本运行时

---

## 目录

1. 设计约束与核心原则
2. 整体架构分层
3. 完整组件清单
4. 核心组件详细设计
5. 硬件资源调度表
6. 文件系统路径约定
7. 启动时序图
8. 错误码全集
9. 附录 A：插件 Manifest 二进制格式 (.mpb)
10. 附录 B：插件开发规范
11. 附录 C：编译与链接选项规范
12. 附录 D：架构准入测试（PoC-A ~ PoC-E）
13. 编码顺序

---

## 1. 设计约束与核心原则

### 1.1 必须遵守的物理现实与信任边界

约束 说明
FATFS 超时 CONFIG_FATFS_TIMEOUT_MS 仅控制获取文件锁的等待时间，不是 SD 卡介质 I/O 超时
PSRAM 访问延迟 约为内部 SRAM 的 3-5 倍，热路径数据结构必须驻留内部 SRAM
内存分配算法 heap_caps_malloc 内部使用 TLSF；频繁加载卸载后仍可能因连续块不足而失败
RTC 内存限制 不支持 LL/SC 原子指令，不能用于高频原子计数器
RTC 保持边界 可跨部分软件复位，但不能抵御完全断电，不属于耐久存储
TF 卡文件系统 必须使用 FAT/exFAT 以确保 PC 兼容性和 esp_elf 加载器稳定运行
内部 Flash 使用 LittleFS 提升可靠性
原生插件隔离 插件与核心共享地址空间；资源句柄和 DI 只能约束可信插件的正确用法，不能抵御恶意原生代码

插件威胁模型:

· 只加载由项目长期发布密钥签名、可信但可能存在缺陷的插件
· 框架负责更新完整性、资源归属、配额、故障检测、旧回调失效和可恢复重启
· 框架不承诺阻止插件主动读写任意可访问内存、调用可见符号或破坏同地址空间状态
· 若未来需要运行不可信第三方代码，必须重新选择具备真实隔离能力的硬件或运行时

### 1.2 不可妥协的铁律

编号 铁律 说明
1 LVGL 单一所有者 只有 Core 0 的 DisplayService 任务可调用 LVGL；插件只能提交不可变显示命令
2 UI 可测 SLA 高优命令成功入队到开始处理 p99 <= 1ms；帧周期 p99 <= 16.67ms，统计方法见附录 D
3 日志有界耐久 BEGIN/END 仅在 fsync 成功且 API 返回成功后承诺抗突然断电；DURING 最多丢最近 1 秒或 4KiB
4 热更新事务性 正常热更新期间核心服务持续运行并支持同 security_epoch 回滚；非协作插件允许受控重启
5 跨核心通信必须经过事件总线 禁止任何插件直接调用另一个插件的函数或共享全局变量
6 所有核心服务必须通过依赖注入（DI）提供给插件 插件不得直接调用全局函数或访问全局变量获取服务
7 中断上下文中禁止使用 publish_copy/publish_buffer/new/delete 中断中仅允许 post_isr，数据传递使用预分配固定块池
8 插件任务中的临界区（自旋锁）持续时长严禁超过 1ms 长时间临界保护必须使用互斥量

---

## 2. 整体架构分层

```text
┌─────────────────────────────────────────────────────────────┐
│  插件层 (ELF 动态加载, 可热更新)                           │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ UI插件   │ │ 网络协议 │ │ 传感器   │ │ 用户逻辑 │     │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘     │
│  所有插件通过 C ABI + 依赖注入调用核心服务                  │
├─────────────────────────────────────────────────────────────┤
│  核心服务层 (C++ 实现，静态链接，不可热更新)               │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────┐ │
│  │ 插件调度器  │ │ 内存服务    │ │ 事件总线            │ │
│  │ (每核Context)│ │ (租约+配额  │ │ (Pub/Sub + 异步RPC) │ │
│  │ 实例Strand  │ │ +空间预检)  │ │ 完成投递到Strand    │ │
│  └─────────────┘ └─────────────┘ └─────────────────────┘ │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────┐ │
│  │ 日志网关    │ │ 网络栈代理  │ │ 资源租约 (GPIO/...) │ │
│  │ (事务性)    │ │ (TCP/UDP)   │ │ (实例+generation) │ │
│  └─────────────┘ └─────────────┘ └─────────────────────┘ │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────┐ │
│  │ System存储  │ │ SD存储      │ │ 插件生命周期管理器  │ │
│  │ (独立Worker)│ │ (独立Worker)│ │ (验证+事务更新)     │ │
│  └─────────────┘ └─────────────┘ └─────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│  HAL 层 (ESP-IDF 原生)                                    │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────┐ │
│  │ heap_caps   │ │ FATFS       │ │ LwIP + WiFi         │ │
│  │ (TLSF)      │ │ SDMMC       │ │ (TCP/UDP)           │ │
│  └─────────────┘ └─────────────┘ └─────────────────────┘ │
│  ┌─────────────┐ ┌─────────────────────────────────────┐ │
│  │ LittleFS    │ │ VFS (虚拟文件系统)                    │ │
│  │ (SPI Flash) │ │ 挂载 /system /plugins /assets /sd    │ │
│  └─────────────┘ └─────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 完整组件清单

组件名 归属层级 核心绑定 实现语言 热更新 依赖注入标识 说明
PluginScheduler 核心服务 Core 0/1 C++ ❌ ctx->sched 每核心一个插件 io_context + 每实例 strand
DisplayService 核心服务 Core 0 C++ ❌ ctx->display LVGL 唯一所有者，双命令队列
LifecycleManager 核心服务 Core 0 C++ ❌ 内部使用 插件状态机与事务更新
MemoryService 核心服务 Core 0 C++ ❌ ctx->mem 租约表 + 配额 + 连续空间预检
EventBus 核心服务 Core 0 C++ ❌ ctx->bus Pub/Sub + 异步 RPC
LogGateway 核心服务 Core 1 C++ ❌ ctx->log 事务性日志（优先级 10）
NetProxy 核心服务 Core 1 C ❌ ctx->net TCP/UDP 异步接口
SystemStorageWorker 核心服务 Core 0 C++ ❌ ctx->storage /system、/plugins、/assets
SDStorageWorker 核心服务 Core 0 C++ ❌ ctx->storage /sd 文件访问与串行恢复
UpdateManager 核心服务 Core 0 C++ ❌ 内部使用 固件 OTA、插件包安装、资源 bundle 与 epoch 提升
ResourceLease 核心服务 Core 0 C++ ❌ ctx->lease GPIO/UART/SPI 管理
PluginLoader 核心服务 Core 0 C++ ❌ 内部使用 ELF 加载、拓扑排序、SemVer
WatchdogManager 核心服务 Core 0 C ❌ 内部使用 plugin_io Worker + 受管任务上下文心跳监控
MetricsCollector 核心服务 Core 0 C++ ❌ ctx->metrics 原子计数器，stats 仅限 UART
FallbackUI 核心服务 Core 0 C++ ❌ — 无 TF 卡时的内置提示
UI 插件 插件层 Core 0 C++ ✅ — 提交显示模型与命令，不直接调用 LVGL
网络协议插件 插件层 Core 1 C++ ✅ — MQTT/HTTP 等
传感器插件 插件层 Core 0 C++ ✅ — I2C/SPI 传感器
用户业务插件 插件层 任意 C++ ✅ — 自定义业务逻辑

---

## 4. 核心组件详细设计

### 4.1 插件调度器 (io_context + strand)

职责：在 FreeRTOS 之上提供 Asio-like 异步调度。FreeRTOS 负责核心亲和性和任务抢占；PluginScheduler 负责有界投递、实例内串行化、实例间公平轮转、定时器、取消、generation 校验和耗时观测。

#### 4.1.1 两层执行拓扑

层级 执行资源 说明
核心服务层 独立 FreeRTOS Worker Display、Lifecycle、SystemStorage、SDStorage、Net、Log 等关键服务不进入插件 io_context
插件调度层 Core 0/1 各一个 plugin_io_context Worker 同一核心上的插件 strand 共享一个 Worker
插件实例层 每实例一个 strand strand 是逻辑串行执行器，不拥有独立 FreeRTOS 任务
显式并行层 按需创建受管任务 仅处理隔离或不可变数据，结果必须 post 回实例 strand

```cpp
plugin_io_context plugin_io_core0(0, 8192, 4, limits_core0);
plugin_io_context plugin_io_core1(1, 8192, 4, limits_core1);

plugin_strand strand = plugin_io_core0.make_strand(
    instance_handle, generation, strand_limits);
```

· 每个 plugin_io_context 由一个固定核心的 FreeRTOS Worker 调用 run()
· 插件 strand 在实例存活期间固定绑定一个核心；新 generation 可在激活前重新选择核心
· Core 0/1 的 plugin_io Worker 使用系统固定的相同低优先级 4；插件不能修改
· 所有 strand 使用所属 Worker 的固定 FreeRTOS 优先级，strand 本身没有优先级
· Manifest 的 priority 不用于 strand；只有经配额批准的额外受管任务可以声明优先级
· 核心服务 Worker 只执行可信核心代码；EventBus、网络、存储和 timer 服务不得直接执行插件函数

#### 4.1.2 strand 投递与顺序语义

· 首版只提供 post，不提供 dispatch 或 defer；handler 永远异步入队，不允许内联重入
· 每个 strand 只有一条 FIFO，统一容纳 POST、EVENT、COMPLETION、CLEANUP 和内部 CONTROL operation
· 所有成功入队的 operation 严格 FIFO，跨生产者顺序以进入 strand intrusive queue 的线性化点为准
· QUIESCING 只允许把尚未执行的 POST 原位改为 CLEANUP，不改变节点位置；其他 operation 不得重排
· 同一 strand 任意时刻最多执行一个 handler，不同 strand 可以分别在 Core 0 与 Core 1 并行
· 普通 post handler 签名为 void(void*)；它不是异步操作 completion，不接收错误码
· I/O、timer 和 RPC completion 使用带 frame_err_t 的独立签名，并保证 exactly once
· 每个 operation 都绑定 instance_handle、generation、类型和销毁函数；执行前必须验证实例状态与 generation
· strand FIFO 使用预分配 operation 节点组成的 intrusive queue；普通 POST 在成功提交时占用一个实例 operation 配额
· 异步操作必须在返回 FRAME_OK 前预留其 completion 节点；IN_FLIGHT 到完成/取消时将同一节点转移进 strand FIFO，不再分配内存
· strand operation 配额、每核心 ready-strand 队列和全局 operation pool 全部有界，容量分别受 Manifest 与系统 Kconfig 限制
· 每个实例在创建 strand 时建立独立 lifecycle control reserve，覆盖 PREPARE、PREPARE_CLEANUP、ACTIVATE、PAUSE、RESUME、EXPORT_STATE、IMPORT_STATE、CANCELLATION_FENCE、QUIESCE 和 UNLOAD 的最大同时在途数；用户配额不得占用
· 每个已创建 strand 在所属核心永久拥有一个 ready slot；ready queue 容量等于该核心允许的最大 strand 数，因此 SCHEDULED 转换不因队列满失败
· 用户 post 在队列或 pool 满时同步返回 FRAME_ERR_QUEUE_FULL，框架不得接管 handler 捕获或数据所有权

#### 4.1.3 ready-strand 公平调度

每核心 Worker 使用 ready-strand 队列轮转；每次只执行一个 strand 的一个 operation：

```text
post(operation)
  -> operation 进入目标 strand FIFO
  -> strand 从 IDLE 原子转换为 SCHEDULED
  -> strand ID 进入所属核心 ready queue

plugin_io_context.run()
  -> 取出一个 ready strand
  -> 取出并执行一个 operation
  -> strand 仍非空：放回 ready queue 尾部
  -> strand 已空：以防丢唤醒协议转换回 IDLE
```

· strand 状态至少包含 IDLE、SCHEDULED、RUNNING、CLOSING、STOPPED
· SCHEDULED 标志保证同一 strand 在 ready queue 中最多出现一次
· RUNNING 转 IDLE 必须与并发 post 使用同一个原子协议；若清空过程中出现新 operation，strand 必须重新入队
· 单个插件持续 post 只能增加自己的 FIFO 压力，不能一次排空 strand 并长期占用 Worker
· 普通插件 handler 必须合作式在 1ms 内返回；单次或连续超限只记录 handler_overrun 指标并输出限频告警
· 超限但最终返回不触发自动隔离或卸载；调度器不尝试抢占、迁移或强制删除调用栈

#### 4.1.4 关联执行器与异步完成

· 发起异步操作时，服务捕获调用实例的 strand handle 和 generation
· 核心服务 Worker 完成 I/O 后，只能通过内部 post_completion 向关联 strand 投递结果
· post_completion 在 QUIESCING 期间仍可用于交付取消结果，但不得绕过 generation 和 exactly-once 状态
· 每个异步 operation 使用原子终态 PENDING、COMPLETED、CANCELLED；底层完成与取消通过 CAS 竞争，先成功者决定 completion 结果
· COMPLETED 先获胜时保留原始结果，即使实例随后进入 QUIESCING；CANCELLED 先获胜时交付 FRAME_ERR_CANCELLED，迟到结果只负责释放底层资源
· completion 使用启动时预留的节点，因此核心服务完成路径不得因 strand FIFO 满而丢失已接收操作的完成通知
· 插件 callback 只在关联 strand 上运行，因此同一实例的事件、I/O completion、timer 和生命周期入口互不并发
· exactly-once 是当前启动周期内且系统未进入 RESTART_REQUIRED 的保证；受控重启不会执行或恢复 RAM 中的插件 callback，耐久 journal 的启动恢复属于新操作而非原 callback 延续
· 可恢复 Worker 错误和 SD RECOVERING 不结束启动周期，也不解除 exactly-once 责任；只有进入 FAILED_RESTART_REQUIRED/RESTART_REQUIRED 并实际终止当前地址空间才结束该责任
· 一个插件 handler 卡死会阻塞所属核心的插件 Worker及同核心其他插件 strand，直到 WDT 触发重启；独立核心服务 Worker 必须继续运行

#### 4.1.5 steady_timer

· 每个 plugin_io_context 提供有界 Asio-like steady_timer，timer 固定绑定创建它的 strand
· timer 状态为 IDLE、ARMED、COMPLETION_QUEUED；每个 timer 同时最多存在一个 async_wait
· timer_async_wait 仅在 IDLE 时成功并预留 completion 节点；重复 wait 返回 FRAME_ERR_BUSY
· timer_expires_after 在没有 outstanding wait 时更新 deadline；ARMED 时调用返回 FRAME_ERR_BUSY，不隐式取消现有 wait
· 到期只向 strand post completion，不在 ESP timer task、ISR 或核心 timer 服务中调用插件代码
· cancel 与到期竞争通过原子完成状态仲裁；PENDING 到 COMPLETED/CANCELLED 的 CAS 先到者决定结果
· timer_cancel 在 ARMED 时发起取消；无 outstanding wait 时返回 FRAME_ERR_NOT_FOUND；重复取消不得产生第二次 completion
· timer_destroy 仅在 IDLE 时成功；ARMED 或 COMPLETION_QUEUED 时返回 FRAME_ERR_BUSY
· deadline 使用单调 64 位微秒时钟；duration 加法溢出返回 FRAME_ERR_INVALID_ARGUMENT
· plugin_io_context 的等待期限取 ready queue 事件与最近 timer deadline 的较早者
· QUIESCING 时取消该实例全部 timer，并将取消 completion 纳入关闭屏障

#### 4.1.6 QUIESCING 与关闭屏障

1. LifecycleManager 带外原子关闭实例的用户 post 入口，并设置 stop token
2. 先通知全部额外受管任务停止并等待退出；任务的最后结果必须在退出确认前使用已预留节点转移到 strand FIFO
3. 向 EventBus（含 RPC）、NetProxy、StorageService 和 steady_timer 发送 cancel_generation(instance, generation, shutdown_epoch)
4. 每个异步源在最后一个已预留 completion 节点转移进 strand FIFO 后，向 LifecycleManager 发送 source_quiesced_ack
5. 尚未开始的普通 POST operation 原位转换为 CLEANUP operation：跳过 invoke，但在原 strand 上调用 destroy
6. CLEANUP 保留原 FIFO 位置，确保 C++ 捕获只在插件代码仍有效且实例串行化成立时析构
7. 已接受异步操作按原子终态竞争结果，使用预留节点向 strand 投递原结果或 FRAME_ERR_CANCELLED completion
8. LifecycleManager 收齐固定异步源集合的 source_quiesced_ack 后，使用预留控制节点在 FIFO 尾部插入 CANCELLATION_FENCE
9. CANCELLATION_FENCE 被执行表示其前方的 COMPLETION 和 CLEANUP 已全部处理；后续状态转换统一按 4.11 执行
10. 当前 handler 或 destroy trampoline 只能合作式退出；超过 1 秒或卡死时不得释放插件内存，记录故障并受控重启

#### 4.1.7 显式受管任务

· 插件不得直接创建 FreeRTOS 任务，只能通过 scheduler_service 申请有配额的受管任务
· 受管任务绑定 instance_handle、generation、stop token、task token、核心、优先级和栈预算
· 受管任务优先级不得高于 plugin_io Worker 的系统优先级 4
· 受管任务只能处理隔离或不可变输入，不得直接访问 strand 所拥有的实例可变状态
· 任务结果、错误和取消结果必须 post 回实例 strand 后才能修改实例状态或调用生命周期逻辑
· 所有阻塞调用必须使用有限超时并检查 stop token；无法在 1 秒内退出时进入受控重启流程

#### 4.1.8 DisplayService 边界与性能契约

· DisplayService 是 Core 0 独立高优先级 Worker，不属于 plugin_io_core0
· 只有 DisplayService Worker 可以调用 lv_* API；插件 strand 只能提交不可变 display_command_t
· 显示命令绑定 instance_handle 和 generation，队列满时返回 FRAME_ERR_QUEUE_FULL
· 目标屏幕为 300x400 RLCD/SPI；高优命令入队到开始处理 p99 <= 1ms，帧周期 p99 <= 16.67ms
· 图片解码、布局计算等重工作必须在受管任务处理，并将不可变结果 post 回 strand 后再提交显示命令
· PoC 必须验证全帧与脏区带宽；若 SPI 不支持全帧 60fps，只允许承诺实测可达的局部刷新范围

#### 4.1.9 自旋锁与故障边界

· strand 提供串行化，不提供故障隔离或抢占能力
· 插件代码中的 portENTER_CRITICAL 临界区严禁超过 1ms，且不得调用任何可能阻塞或分配内存的 API
· 关闭中断、死循环或不返回 handler 由 Interrupt WDT/Task WDT 触发整机恢复
· 框架不得通过 vTaskSuspend、vTaskDelete 或提升故障任务优先级尝试恢复

---

### 4.2 内存服务 (MemoryService)

核心绑定: Core 0

#### 4.2.1 物理内存分配

区域 策略 说明
插件代码/数据 PSRAM generation arena 每个实例独立、地址不可移动
插件 IRAM 专用区 按 ELF 专用 section 分配，卸载后归还
系统动态区 ESP-IDF capability heap Wi-Fi、LwIP、TLS、文件系统和 DMA 使用
内部 SRAM 核心保留 任务栈、队列、LVGL DMA 缓冲和关键元数据

· 11.5MiB 仅作为待验证的插件总配额上限，不代表启动时实际预留出独占物理池
· 若 PoC 不能证明普通 capability heap 可稳定满足旧/新实例共存，则必须建立专用 PSRAM arena 后才能承诺该配额
· 所有返回给插件的指针在实例存活期间保持地址稳定，框架不得搬移

#### 4.2.2 配额管理

分配接口 物理介质 配额控制
alloc_image(size) PSRAM 计入 max_memory_bytes
alloc_packet(size) PSRAM 计入 max_memory_bytes
alloc_generic(size) PSRAM（强制） 计入 max_memory_bytes
内部 SRAM 仅限核心服务 插件禁止使用

· Manifest 不允许插件直接申请任意内部 SRAM
· 需要 DMA 或外设专用内存时，通过 ResourceLease 的受限资源类型申请，由核心决定 capability 和上限
· 每次分配绑定 instance_handle 与 generation，旧 generation 不能释放新实例内存

#### 4.2.3 租约表

参数 值
最大条目 2048（可 Kconfig 配置）
内存位置 内部 SRAM
占用 40KB
保护 portMUX_TYPE 自旋锁

```cpp
typedef struct {
    void* ptr;          // 内存块指针
    uint32_t instance_id;
    uint32_t generation;
    size_t size;        // 块大小（字节）
    uint32_t caps;      // 分配属性
    uint16_t next_free; // 空闲链表索引
} lease_t;
```

#### 4.2.4 连续空间监控与加载预检

最小连续空闲块监控：

· 仅在插件加载/更新前通过 heap_caps_get_info 获取 PSRAM 最大连续空闲块快照
· 不在周期任务或 UI 路径轮询，不把 heap_caps_check_integrity 当作整理或回收手段
· 不实现运行时内存搬移；连续空间不足时拒绝候选包

加载器必须先解析 ELF Program Header，再计算：

· 所有 PT_LOAD 段的 p_memsz、p_align 和段间填充
· BSS、重定位表、loader 元数据、plugin_context_t 和受管任务栈
· IRAM 专用 section 的真实大小
· 热更新时旧 ACTIVE generation 与候选 generation 的共存空间
· 经 PoC 校准的安全余量

任一物理区域或配额不足时，在执行任何插件代码前返回 FRAME_ERR_MEM_FRAGMENTED 或 FRAME_ERR_QUOTA_EXCEEDED。

#### 4.2.5 包大小与运行时预算

· CONFIG_FRAME_MAX_MPB_BYTES 是整个 .mpb 文件的系统级 Kconfig 上限，只用于存储和解析防护，不是 Manifest TLV
· max_memory_bytes 限制单 generation 的代码、数据和框架代分配总量
· iram_required_bytes 必须与 loader 对专用 ELF section 的计算值一致
· 不使用 ELF 文件物理大小推断运行时内存，也不假设固定 512KiB 回滚区足够

#### 4.2.6 回收接口

```cpp
frame_err_t reclaim_instance(plugin_instance_handle_t instance);
```

只有 LifecycleManager 同时确认以下条件后，才能调用 reclaim_instance：实例 strand 已执行 CANCELLATION_FENCE，所有 completion/CLEANUP 已处理，额外受管任务已退出，UNLOAD operation 已返回且 strand 状态为 STOPPED。

---

### 4.3 事件总线 (EventBus)

核心绑定: Core 0

#### 4.3.1 接口定义

```c
typedef uint64_t frame_buffer_handle_t;
typedef uint64_t subscription_handle_t;

typedef struct {
    const void* data;
    size_t len;
    uint32_t source_instance_id;
    uint32_t source_generation;
    uint32_t correlation_id;
} event_view_t;

typedef void (*event_handler_t)(void* user, const event_view_t* event);
typedef void (*rpc_completion_t)(void* user, frame_err_t err,
                                 const event_view_t* response);

typedef struct event_bus {
    frame_abi_header_t abi;
    frame_err_t (*publish_copy)(const char* topic, const void* data, size_t len);
    frame_err_t (*buffer_alloc)(size_t len, frame_buffer_handle_t* out_handle,
                                void** out_data);
    frame_err_t (*publish_buffer)(const char* topic, frame_buffer_handle_t handle);
    frame_err_t (*buffer_release)(frame_buffer_handle_t handle);
    frame_err_t (*subscribe)(const char* topic, event_handler_t handler, void* user,
                             subscription_handle_t* out_subscription);
    frame_err_t (*request)(const char* topic, const void* req, size_t req_len,
                           rpc_completion_t completion, void* user, uint32_t timeout_ms);
    frame_err_t (*respond)(uint32_t correlation_id, const void* resp, size_t resp_len);
    frame_err_t (*unsubscribe_all)(void);
} event_bus_t;
```

服务表由框架按插件实例创建，内部已绑定 instance ID 和 generation。插件 API 不接受可伪造的 plugin_id 参数。

EventBus 只路由和管理引用，不直接调用插件 handler。发布事件、RPC 响应和 RPC 超时都转换为关联 strand 的 operation：普通事件使用 EVENT 类型，RPC 使用带 frame_err_t 的 COMPLETION 类型。

#### 4.3.2 消息所有权规则

· publish_copy 在调用返回前复制数据；返回后调用者可立即释放原数据
· buffer_alloc 从 MemoryService 创建 opaque、引用计数缓冲，裸数据指针只在首次发布前可写
· publish_buffer 返回 FRAME_OK 后总线接管调用者持有的一个引用；失败时所有权仍归调用者
· 每个订阅队列持有独立引用，多订阅者、同核和跨核使用同一规则
· 无订阅者时 publish_buffer 可返回 FRAME_OK，并由总线立即释放接管的引用
· callback 返回后由总线释放该订阅引用；订阅者不得缓存 event_view_t 或 data 指针
· 队列满时只丢弃对应订阅者的投递，递增 event_dropped，并正确释放该订阅引用
· 所有消息、订阅、RPC 和 timeout 都带 generation；目标不再 ACTIVE 时完成一次取消或丢弃，旧代码不得执行
· 热更新 ingress gate 位于 generation 路由之前；目标处于 PAUSING/PAUSED 时，只有核心 schema 标记为 replayable 的不可变 EventBus/UI/控制输入可按全局到达序号进入预留延迟队列
· 延迟项持有复制数据或 opaque buffer 引用，不持有旧 generation callback；RPC、timer、网络/存储 completion 和外设句柄事件不得延迟或跨 generation 回放
· 非 replayable 新请求在 PAUSING/PAUSED 时同步返回 FRAME_ERR_BUSY；延迟项只在恢复旧实例或提交新实例时绑定目标 generation 并按序投递
· RPC handler 通过 event_view_t.correlation_id 获取请求 ID；响应和超时竞争通过原子状态保证 callback exactly once
· QUIESCING 时，尚未执行的 EVENT operation 直接销毁并释放消息引用；未完成 RPC 必须以 FRAME_ERR_CANCELLED 向 strand exactly once 完成

#### 4.3.3 中断上下文限制

操作 是否允许
post_isr ✅ 仅允许投递固定块池索引，队列满则归还块并递增 tasks_dropped
publish_copy / publish_buffer ❌ 禁止
MemoryService 系列 ❌ 禁止
StorageService 系列 ❌ 禁止
new / delete ❌ 禁止

中断中数据传递使用预分配固定块池和 SPSC/FromISR 队列，禁止多个中断共享一个可覆盖的全局缓冲区（详见 4.13）。

#### 4.3.4 防风暴机制

参数 默认值
默认阈值 100 次/秒
`sensor/*` 主题 200 次/秒
ui/* 主题 100 次/秒
Manifest 可配置 event_rate_limit

---

### 4.4 日志系统 (LogGateway)

核心绑定: Core 1
任务优先级: 10

#### 4.4.1 事务模型

操作 行为
BEGIN 向 /system/session journal 追加 BEGIN 记录，fsync 成功后返回 session_id
DURING 写入 64KiB PSRAM 缓冲；1 秒或累计 4KiB 先到即批量刷新
END 先提交待处理 DURING 批次，再向 journal 追加 END 记录；全部 fsync 成功后返回

· BEGIN/END API 返回 FRAME_OK 后，记录必须能在突然断电后恢复
· API 返回前断电、底层返回错误或 /system 不可用时，不承诺该操作成功
· DURING 在正常持久化链路下最多丢失最近 1 秒或 4KiB；缓冲溢出时丢弃最旧完整记录并计数
· session journal 使用 append-only CRC 记录或 A/B 双槽；每条记录包含 format、sequence、session_id、operation、payload hash 和 CRC

#### 4.4.2 持久化通道与完成语义

数据 通道 说明
BEGIN/END /system/session 必须由 SystemStorageWorker 写入并 fsync 后完成
DURING 首选 /sd/logs 由 SDStorageWorker 批量追加
DURING 降级 /system/logs SD 不可用时继续批量追加，执行容量配额和循环淘汰
紧急遥测 UDP 非阻塞 best effort，不参与耐久确认
复位提示 RTC 仅保存 session_id、sequence 和最小故障码，不能抵御完全断电

LogGateway 对每个 BEGIN/END 请求保持单一完成状态。存储完成、超时和取消相互竞争时，只允许向调用方完成一次。

#### 4.4.3 SD 故障下的日志行为

· SD 状态不是 READY 时，新 DURING 批次直接路由到 /system/logs，不等待 SD 恢复
· 已交给 SDStorageWorker 的批次必须由该 Worker exactly once 完成成功或失败；失败后 LogGateway 才可重投 /system
· /system/logs 配额默认 256KiB，按完整批次淘汰最旧文件，不得覆盖 session journal、证书或故障记录
· /system 也不可用时，只保留 UDP + PSRAM best effort，并上报 LOG_DURABILITY_DEGRADED；此状态不满足 DURING 有界丢失 SLA

#### 4.4.4 日志路径降级

条件 路径
正常 /sd/logs/
SD 不可用 /system/logs/
SystemStorage 不可用 仅 UDP + PSRAM，RTC 保存最小提示

#### 4.4.5 启动恢复顺序

1. 挂载 /system，并扫描 session journal 中 sequence 最大且 CRC 有效的记录
2. 若最新会话存在 BEGIN 而没有匹配 END，生成 ABNORMAL_TERMINATION 待上报记录
3. 初始化 LogGateway，写入新的启动会话 BEGIN 并确认落盘
4. 网络可用后发送待上报记录；发送成功和本地 journal 提交成功后才清理旧异常标记
5. RTC 内容只用于补充判断软件复位窗口，不得覆盖 journal 结论，也不得在恢复完成前清空

---

### 4.5 存储服务 (StorageService)

核心绑定: Core 0
SystemStorageWorker 优先级: 9
SDStorageWorker 优先级: 8

#### 4.5.1 设计目标

· 统一异步文件访问接口
· /system、/plugins、/assets 由 SystemStorageWorker 串行访问
· /sd 由独立 SDStorageWorker 串行访问和恢复
· SD 故障、缺卡或长时间 I/O 不得阻塞内部 Flash 请求
· 插件只使用异步接口；BEGIN/END、注册表和更新事务通过核心内部耐久写接口完成

#### 4.5.2 C ABI 接口

```c
typedef uint64_t storage_request_handle_t;

typedef void (*storage_callback_t)(void* user, frame_err_t err, const uint8_t* data, size_t len);

typedef struct storage_service {
    frame_abi_header_t abi;
    frame_err_t (*read)(const char* path, uint32_t offset, uint32_t len,
                        storage_callback_t cb, void* user,
                        storage_request_handle_t* out_request);
    frame_err_t (*write)(const char* path, const uint8_t* data, size_t len,
                         storage_callback_t cb, void* user,
                         storage_request_handle_t* out_request);
    frame_err_t (*stat)(const char* path, storage_callback_t cb, void* user,
                        storage_request_handle_t* out_request);
    frame_err_t (*unlink)(const char* path, storage_callback_t cb, void* user,
                          storage_request_handle_t* out_request);
    frame_err_t (*cancel)(storage_request_handle_t request);
} storage_service_t;
```

· 服务表已绑定调用实例和 generation，插件不能访问其他插件的私有路径
· callback 的 data 仅在 callback 返回前有效；需要长期持有时由插件自行复制
· write 在返回 FRAME_OK 前复制输入数据或接管明确的 buffer handle，不得异步保存调用者裸指针
· 发起请求时捕获关联 strand；SystemStorageWorker 和 SDStorageWorker 只产生结果，再通过 post_completion 交付 callback
· callback 不得在存储 Worker 上执行，存储 Worker 不等待插件 callback 返回

#### 4.5.3 路径路由与 Worker 状态

路径 Worker 文件系统
`/system/*` SystemStorageWorker LittleFS，配置、证书、journal、故障与降级日志
`/plugins/*` SystemStorageWorker LittleFS，Fallback 与已安装插件包
`/assets/*` SystemStorageWorker LittleFS，只读资源
`/sd/*` SDStorageWorker FAT/exFAT，外部插件、资源与首选日志

每个 Worker 独立维护状态：UNINITIALIZED、READY、FAILED、RECOVERING；SD Worker 额外支持 ABSENT。一个 Worker 的状态不得阻塞另一个 Worker 消费队列。

#### 4.5.4 队列策略与请求状态

队列 容量 用途
System 高优先级队列 16 BEGIN/END、注册表、故障和更新 journal
System 普通队列 16 配置、插件和资源访问
SD 高优先级队列 16 DURING 日志批次
普通队列 32 插件发起的常规文件读写

请求状态严格按以下方向转换：

CREATED → QUEUED → IN_FLIGHT → COMPLETED
                     └────────→ CANCELLED

· API 返回 FRAME_OK 表示请求已被接收，不表示 I/O 已完成
· API 在入队失败时同步返回错误且绝不调用 callback
· 在当前启动周期未进入 RESTART_REQUIRED 时，入队成功后必须且只能调用 callback 一次，包括取消、插件卸载和可恢复 Worker 错误
· cancel 只能使 QUEUED 请求取消；IN_FLIGHT 请求标记 cancel_requested，底层返回后以 CANCELLED 完成
· 不能通过删除 Worker 任务取消同步驱动调用

两个 Worker 使用同一公平策略：每处理最多 8 个高优请求，必须给普通队列一次机会；无任务时通过 Queue Set 或任务通知等待任一队列。

#### 4.5.5 SD 超时检测与恢复边界

· CONFIG_FATFS_TIMEOUT_MS 只限制 FatFs 获取文件锁的等待时间，不用于判断 SD 介质无响应
· GPTimer 或 esp_timer 可记录 deadline 并触发监控事件，但不能中断已经进入的 FatFs/SDMMC 同步调用
· 具体 SDMMC 命令超时、host reset 和重新挂载 API 必须按 ESP-IDF v6.0.2 PoC 结果实现和记录
· SD 恢复只允许由 SDStorageWorker 在当前驱动调用已经返回后串行执行：关闭文件 → 卸载 VFS → deinit host → 重新 init → 挂载 → 健康检查
· 不创建独立恢复任务；禁止任何其他任务与 IN_FLIGHT 调用并发执行 sdmmc_host_deinit
· 调用在 WDT 时限内不能返回时，标记 STORAGE_RESTART_REQUIRED，写入 RTC 最小故障码并受控重启
· 恢复期间新的 /sd 请求立即返回 FRAME_ERR_FS_RECOVERING；内部 Flash 请求继续正常服务
· SD 恢复不持有 SystemStorageWorker 的队列、VFS 或文件系统锁；该 Worker 继续处理 /system、/plugins 和 /assets
· SystemStorageWorker 自身调用超时或 WDT 时不尝试用 SD 恢复流程处理，而是将系统标记 RESTART_REQUIRED；若无法写 /system journal，只保留 RTC 最小故障标记

#### 4.5.6 Worker 伪代码

```c
#define MAX_PRIORITY_BATCH 8

while (1) {
    wait_for_storage_work();
    int high_processed = 0;
    while (high_processed < MAX_PRIORITY_BATCH) {
        if (xQueueReceive(priority_q, &req, 0) == pdTRUE) {
            process_and_complete_once(req);
            high_processed++;
        } else break;
    }

    if (xQueueReceive(normal_q, &req, 0) == pdTRUE) {
        process_and_complete_once(req);
    }
}
```

---

### 4.6 网络服务 (NetProxy)

核心绑定: Core 1
任务优先级: 12

```c
typedef uint64_t net_connection_handle_t;
typedef uint64_t net_listener_handle_t;
typedef uint64_t net_socket_handle_t;
typedef uint64_t net_request_handle_t;

typedef struct {
    const uint8_t* data;
    size_t len;
} net_receive_view_t;

typedef void (*net_connect_callback_t)(void* user, frame_err_t err,
                                       net_connection_handle_t connection);
typedef void (*net_listen_callback_t)(void* user, frame_err_t err,
                                      net_listener_handle_t listener);
typedef void (*net_accept_callback_t)(void* user, frame_err_t err,
                                      net_connection_handle_t connection);
typedef void (*net_io_callback_t)(void* user, frame_err_t err, size_t transferred);
typedef void (*net_receive_callback_t)(void* user, frame_err_t err,
                                       const net_receive_view_t* view);

typedef struct network_service {
    frame_abi_header_t abi;
    frame_err_t (*tcp_connect)(const char* host, uint16_t port,
                               net_connect_callback_t cb, void* user,
                               net_request_handle_t* out_request);
    frame_err_t (*tcp_send_copy)(net_connection_handle_t connection,
                                 const void* data, size_t len,
                                 net_io_callback_t cb, void* user,
                                 net_request_handle_t* out_request);
    frame_err_t (*tcp_recv)(net_connection_handle_t connection, size_t max_len,
                            net_receive_callback_t cb, void* user,
                            net_request_handle_t* out_request);
    frame_err_t (*tcp_close)(net_connection_handle_t connection,
                             net_io_callback_t cb, void* user,
                             net_request_handle_t* out_request);
    frame_err_t (*tcp_listen)(uint16_t port, net_listen_callback_t cb, void* user,
                              net_request_handle_t* out_request);
    frame_err_t (*tcp_accept)(net_listener_handle_t listener,
                              net_accept_callback_t cb, void* user,
                              net_request_handle_t* out_request);
    frame_err_t (*udp_create)(uint16_t local_port, net_socket_handle_t* out_socket);
    frame_err_t (*udp_send_copy)(net_socket_handle_t socket, uint32_t ipv4,
                                 uint16_t port, const void* data, size_t len);
    frame_err_t (*cancel)(net_request_handle_t request);
} network_service_t;
```

· tcp_send_copy 和 udp_send_copy 在返回 FRAME_OK 前复制输入；失败时不保留调用者指针
· tcp_recv 不接受插件缓冲区；NetProxy 从核心有界接收池提供只读 net_receive_view_t，其 data 仅在 callback 返回前有效
· tcp_accept 和 tcp_recv 都是一次性操作；每个 listener 或 connection 同时只允许一个同类未决操作，callback 后由插件显式 rearm
· 所有异步请求在返回 FRAME_OK 前预留 completion 节点和所需核心缓冲；不能预留时返回 FRAME_ERR_CAPACITY，底层操作不得启动
· tcp_send_copy/udp_send_copy 按输入 len，tcp_recv 按请求 max_len 同时从实例 max_network_buffer_bytes 与系统网络池预留；max_len 必须 <= max_network_io_bytes
· 多 connection 并发时以缓冲预留成功的线性化顺序接纳，失败请求不占资源。recv callback 返回后归还接收块；callback 内 rearm 需要另一份可用预算，否则同步返回 FRAME_ERR_CAPACITY

并发限制：

· 最大 TCP 连接数：8（含监听套接字）
· 最大 UDP 套接字数：4
· 每实例最大未决网络请求、单次发送/接收长度和接收池总字节数由 Manifest 配额限制，并受系统 Kconfig 上限约束

回调执行上下文：

· NetProxy Worker 固定 Core 1，只执行核心网络状态机，不直接调用插件函数
· 发起操作时捕获实例 strand 与 generation；成功、错误、超时和取消都通过 post_completion exactly once 交付
· listener 和 connection 属于持续资源，但 accept/recv 是一次性异步操作；QUIESCING 时先禁止 rearm，再以完成/取消 CAS 的胜者结果结算未决操作
· 插件网络 callback 在实例 strand 上执行；需要更新 UI 时提交 DisplayService 命令，禁止调用 LVGL API

---

### 4.7 可观测性 (MetricsCollector)

```cpp
typedef struct {
    atomic_uint32_t tasks_dropped;              // 调度器丢弃任务数
    atomic_uint32_t strand_post_rejected;       // 实例/全局 operation 配额耗尽
    atomic_uint32_t strand_handler_overrun;     // handler 执行超过 1ms
    atomic_uint32_t strand_cleanup_count;       // QUIESCING 转换的 CLEANUP 数量
    atomic_uint32_t completion_cancelled;       // 取消竞争获胜的 completion 数量
    atomic_uint32_t display_commands_dropped;   // 显示命令队列满
    atomic_uint32_t stale_generation_dropped;   // 丢弃旧 generation 回调
    atomic_uint32_t logs_udp_dropped;           // UDP 日志发送失败
    atomic_uint32_t logs_ring_overflow;         // 循环缓冲区覆盖
    atomic_uint32_t mem_alloc_fail;             // 内存分配失败
    atomic_uint32_t lease_overflow;             // 租约表溢出
    atomic_uint32_t event_dropped;              // 事件总线丢弃
    atomic_uint32_t plugin_load_fail;           // 插件加载失败
    atomic_uint32_t wdt_timeout_count;          // 看门狗超时
    atomic_uint32_t restart_required_count;      // 无法安全卸载而重启
    atomic_uint32_t storage_recovery_count;      // SD 恢复次数
    atomic_size_t   max_free_block_size;        // 最大连续空闲块
} metrics_t;
```

查询接口：

· stats 命令仅绑定到 UART 控制台（esp_console）
· 不开放给网络接口，防止信息泄露

---

### 4.8 资源租约 (ResourceLease)

核心绑定: Core 0

```c
typedef enum {
    RES_GPIO, RES_UART, RES_SPI, RES_I2C, RES_TIMER, RES_DMA
} resource_type_t;
typedef uint64_t resource_handle_t;

typedef struct resource_lease {
    frame_abi_header_t abi;
    frame_err_t (*acquire)(resource_type_t type, uint32_t id,
                           const void* config, size_t config_size,
                           resource_handle_t* out_handle);
    frame_err_t (*release)(resource_handle_t handle);
    frame_err_t (*release_all)(void);
    bool (*is_occupied)(resource_type_t type, uint32_t id);
    bool (*is_owned)(resource_type_t type, uint32_t id);
} resource_lease_t;
```

· 服务表由框架按实例生成，插件不能覆盖或伪造 instance_handle
· 每个租约内部记录 instance ID、generation 和资源状态
· QUIESCING 后拒绝新 acquire；正常卸载时由 LifecycleManager 调用 release_all
· 上述 release_all(void) 只释放当前绑定实例；核心另有不进入插件服务表的 release_all(instance, generation)，仅 LifecycleManager 可调用
· FAILED_RESTART_REQUIRED 时不强制释放硬件资源，由整机重启恢复外设状态

---

### 4.9 看门狗管理器 (WatchdogManager)

核心绑定: Core 0
任务优先级: 22

#### 4.9.1 机制

· Core 0/1 的 plugin_io_context Worker 分别注册 Task WDT，并且只在 operation 之间喂狗
· Display、Lifecycle、Update、SystemStorage、SDStorage、Net 和 Log 等关键核心 Worker 也分别注册 Task WDT；故障快照使用核心 Worker ID，不伪造插件 generation
· Worker 调用插件 operation 前记录当前 instance_handle、generation、strand 和 operation 类型，返回后清除
· 普通 strand handler 不需要也不能主动喂狗；运行超过 1ms但最终返回时记录 handler_overrun
· 不返回的 strand handler 会阻止所属 plugin_io_context Worker 喂狗，WDT 可据当前 operation 归因到具体实例
· 额外受管任务单独注册 Task WDT，并使用不可伪造的 task token 喂狗
· Manifest 的 heartbeat_interval_ms 仅作用于额外受管任务，不改变 strand Worker 的系统 WDT 配置
· 锁定的 ESP-IDF 版本必须提供可用的 Task WDT 第一阶段 hook；hook 只写预分配故障快照并用该上下文允许的非阻塞原语通知 WatchdogManager，不获取普通锁、不访问文件系统、不调用插件代码
· 进入第一阶段处理时立即建立 1 秒绝对恢复截止时间；故障 plugin_io Worker 或受管任务不能延长该截止时间，超时由独立系统复位路径进入 panic/restart
· 若目标 ESP-IDF 无法证明第一阶段 hook 可安全通知 Worker，则 Task WDT 也按 4.9.3 panic 路径处理，不声称可合作关闭

#### 4.9.2 Task WDT 超时处理

1. 高优先级 WatchdogManager 收到第一阶段通知后记录 wdt_timeout_count++；所有后续等待都受同一个 1 秒绝对恢复截止时间约束
2. 若超时来自额外受管任务，带外关闭实例用户 post 入口并设置全部 stop token
3. 若受管任务在 1 秒内全部退出且实例 strand Worker 健康，按 4.1.6 和 4.11 执行正常关闭
4. 若超时来自任意 strand 生命周期/用户 handler，或任一受管任务未退出，直接将实例标记 FAILED_RESTART_REQUIRED；PAUSE、RESUME、QUIESCE 和 UNLOAD handler 不例外
5. 若超时来自关键核心 Worker，将系统标记 RESTART_REQUIRED，不把故障错误归因给当前插件
6. LifecycleManager 不向已阻塞 strand 排队恢复逻辑，也不修改故障任务优先级
7. 核心 SystemStorageWorker 尝试将包 SHA-256、security_epoch、version、generation 和当前 operation 写入 /system/faults journal
8. 仅当 LifecycleManager、SystemStorageWorker 和调度所需核心任务仍健康时等待 journal 落盘；否则跳过耐久写入，仅保存 RTC 最小故障标记
9. journal 确认落盘或核心协调路径判定不可用后调用 esp_restart()
10. 下次启动隔离被明确归因的 generation；核心 Worker 故障无可靠插件归因时进入安全模式，不任意隔离最后运行的插件

#### 4.9.3 Interrupt WDT 与 panic 路径

· Interrupt WDT 表示中断被长时间屏蔽或 tick 无法运行，直接进入 ESP-IDF panic handler，不执行 4.9.2 的合作关闭
· panic 上下文不得调用 LifecycleManager、向 strand post、等待任务退出、获取普通锁或执行 LittleFS/FatFs fsync
· panic 路径只写入预留 RTC/低级 panic 记录，包含核心、当前 instance、generation 和 operation 的最佳努力快照，然后由配置的 panic 策略重启
· coredump 只有在锁定版本的 ESP-IDF 证明目标后端可用于该 panic 场景时才启用；其成功不是重启或 generation 隔离的前提
· 启动恢复将 RTC/panic 标记合并进 /system/faults journal，并隔离故障 generation；若标记不完整则进入保守安全模式

---

### 4.10 插件框架 (PluginLoader)

核心绑定: Core 0

#### 4.10.1 插件加载流程

1. 选择候选源：正常包来自 /sd/plugins/*.mpb，Fallback 包来自 /plugins/fallback.mpb
2. 将完整 .mpb 读入只读 staging buffer；后续解析、哈希和 ELF 加载必须使用同一批字节
3. 按附录 A 做溢出、边界、重叠、重复 payload、目标平台和长度检查
4. 使用固化公钥验证包签名，再验证 ELF 和资源 payload 的 SHA-256
5. 比较 security_epoch 与 eFuse floor：低于 floor 立即拒绝，同 epoch 内允许普通版本回滚
6. 校验核心 ABI major/minor、feature bits 和目标 esp32s3；不兼容返回 FRAME_ERR_ABI_MISMATCH
7. 构建依赖图并拓扑排序：
   · 构建 DAG，节点为插件，边为 requires
   · 使用 Kahn 算法拓扑排序
   · 若存在环 → FRAME_ERR_DEP_CYCLE
8. 语义化版本依赖解析：
   · requires 支持版本约束语法："lib_base>=2.0.0,<3.0.0"
   · 解析已加载插件的 version_str 为三元组（major, minor, patch）
    · 不满足 → FRAME_ERR_SEMVER_MISMATCH
9. 解析 ELF Program Header、section 和 relocation，仅接受 PoC-A 白名单中的类型；loader 计算 .plugin_iram section 实际大小并要求严格等于 Manifest.iram_required_bytes，不一致返回 FRAME_ERR_PACKAGE_INVALID
10. 按 4.2.4 计算候选 generation 的 PSRAM、IRAM、栈和旧/新共存需求；大小匹配但物理 IRAM 不足时返回 FRAME_ERR_IRAM_EXHAUSTED
11. 仅创建记录包摘要、版本和依赖结果的 STAGED 注册项；此状态尚未分配可执行段或调用插件代码
12. 原子转为 LOADING，创建 generation arena 和绑定目标核心的实例 strand，复制并重定位已验证字节，完成 cache 同步和 .bss 清零
13. 构造只包含内存、诊断和自检能力的 prepare_context_t，由 LifecycleManager 向实例 strand 投递内部 PREPARE operation
14. 实例 strand 调用 plugin_prepare 并向 LifecycleManager post 核心完成事件；成功后原子转为 PREPARED，才可进入热更新提交或首次激活流程
15. LifecycleManager 不直接调用任何插件入口；失败时按 4.10.2 逆序清理

#### 4.10.2 加载失败的内存清理

1. 若实例 strand 健康且 prepare 已进入可清理状态，向该 strand 投递内部 PREPARE_CLEANUP operation
2. cleanup completion 返回后关闭并排空候选 strand
3. 释放候选任务描述、prepare_context_t 和 generation arena
4. 释放候选 IRAM，并按实际构造阶段销毁 LOADING、PREPARED 或 STAGED 注册项
5. staging buffer 引用归零后释放
6. 记录 plugin_load_fail++ 和精确失败阶段
7. 返回原始错误码，不改变当前 ACTIVE generation

#### 4.10.3 插件 ELF 执行位置

段 目标位置 准入条件
.text PSRAM PoC-A 证明动态分配地址可执行、重定位正确且 cache 同步有效
.rodata PSRAM 必须保持只读语义或由 loader 在 ACTIVE 后禁止写入
.data / .bss PSRAM 位于当前 generation arena
.plugin_iram IRAM loader 校验 section、大小、对齐和 relocation 后复制

文档不依赖 CONFIG_SPIRAM_XIP_FROM_PSRAM。ESP-IDF 的静态固件指令搬运选项不能证明任意动态 ELF 分配区可执行；实际配置名和 loader commit 必须由 PoC-A 固定。

#### 4.10.4 IRAM 热函数空间管理

· 插件工具链使用专用 .plugin_iram ELF section，不使用函数名字符串猜测地址
· Manifest 的 iram_required_bytes 必须等于 loader 根据 section 计算的值
· 框架按 generation 维护 IRAM allocation，加载失败和正常卸载时归还
· IRAM 不足时在执行插件代码前返回 FRAME_ERR_IRAM_EXHAUSTED

#### 4.10.5 无 TF 卡的降级方案

优先级 方案
1 TF 卡可用 → 正常加载
2 TF 不可用或无可用正常包 → /plugins/fallback.mpb
3 Fallback 不存在或验证失败 → DisplayService 内置 FallbackUI 场景

#### 4.10.6 热更新事务

Activation-staging 协议：

· 候选 generation 拥有核心内部 activation transaction、按调用顺序记录的 intent log 和 completion delivery barrier；插件 ABI 不暴露或允许修改 transaction ID
· staging 服务表保持与正常服务表相同的 ABI，但每个入口先校验身份、参数、依赖顺序、实例配额和物理池余量，再复制输入并预留 handle、operation、buffer 或租约；返回 FRAME_OK 后所有权转移给 transaction
· staging handle 状态为 ACTIVATION_RESERVED，只能在同一 ACTIVATE 调用中用于构造后续依赖 intent；在事务提交前不能访问底层资源，也不能被其他 generation 使用
· intent 不启动 Worker、timer、I/O、显示命令、日志写入或外设配置；MemoryService 对候选 arena 的普通分配和只读自检查询可立即执行，因为它们随候选整体回收且不产生外部副作用
· 任一 staging 调用失败时，该次调用不转移所有权；ACTIVATE 可返回失败。事务中止时，已接管的普通 POST 在候选 strand 转为 CLEANUP，已接管的异步 completion 以 FRAME_ERR_CANCELLED exactly once 结算，handle 失效，随后按 intent 逆序释放预留并执行 PREPARE_CLEANUP
· durable commit 与 ingress 切换成功后，所有 intent 按记录顺序原位转为 LIVE 并提交各核心 Worker；全部提交完成前 delivery barrier 禁止候选 callback 获得执行。底层操作此后失败属于新 ACTIVE generation 的普通异步结果
· 预留保证正常提交不再分配且不能因容量失败；若提交阶段发现内部不变量破坏，状态转为 FAILED_RESTART_REQUIRED，保留当前地址空间资源并受控重启

1. 完成候选包签名、依赖、内存预检和 strand 上的 plugin_prepare，候选进入 PREPARED
2. 按系统上限及旧/候选 Manifest 的 max_update_backlog_events/max_update_backlog_bytes 较小值，预留本次事务所需的暂停控制节点、可转移 EVENT operation 节点和 payload 空间；同时验证这些节点可作为目标 strand 的事务专用额外 credit。容量为 0 或预留失败时保持旧实例 ACTIVE 并返回 FRAME_ERR_CAPACITY
3. 原子地将旧实例从 ACTIVE 转为 PAUSING 并安装可逆 ingress gate；EventBus、UI 和控制面等框架可重放输入按全局到达序号填入预留 EVENT 节点，不再调用旧实例
    · transaction backlog 位于 generation 路由之外，不是旧 generation 的异步 producer，不阻止其 source ACK 或 fence；节点只在 RESUME 或候选提交时整体拼接到目标 strand
4. 停止旧实例的额外受管任务并等待退出；任务最终结果必须在退出确认前转移到旧 strand
5. 按 4.15.1 对旧 generation 执行 cancel_generation(instance, generation, shutdown_epoch)，等待旧 I/O completion 在旧 strand 按原结果或 FRAME_ERR_CANCELLED 结算，并收齐匹配 shutdown_epoch 的固定异步源 source_quiesced_ack
6. 在旧 strand FIFO 尾部插入 CANCELLATION_FENCE；fence 到达后执行内部 PAUSE operation，形成一致快照并进入 PAUSED
7. 在旧 strand 执行 EXPORT_STATE，将带 schema ID、schema version、长度和 CRC 的快照交给候选 strand 执行 IMPORT_STATE。旧/候选都声明无状态时跳过；仅一方无状态或 schema ID 不同则返回 FRAME_ERR_STATE_SCHEMA
8. import、activate 或后续提交前检查失败时，丢弃候选尚未释放的服务意图并执行 PREPARE_CLEANUP；随后在旧 strand 执行 RESUME，重建受管任务和已取消的 I/O 注册；成功后在路由锁下将延迟 EVENT 节点重绑定并整体拼接到旧 strand FIFO，再原子恢复 ACTIVE 和移除 ingress gate
9. import 成功后，为候选安装 activation-staging 服务视图并执行 ACTIVATE；服务调用只校验配额、复制数据和预留 handle/operation，不启动 Worker、受管任务、I/O、timer、显示命令或 callback。ACTIVATE 成功后候选仍为 PREPARED；候选声明的新连接和存储操作不迁移旧 generation 的 socket、文件句柄或 completion
10. 将 active generation、包摘要和回滚 generation 原子提交到 /system/plugin_registry journal；随后在路由锁下将全部延迟 EVENT 节点重绑定并按到达序号整体拼接到候选 strand FIFO，再以同一线性化点切换 ingress gate 的目标 generation、将候选转为 ACTIVE 并开放新输入；最后才将已预留的 activation service intents 释放给各核心 Worker
11. 提交前延迟队列溢出时立即中止更新并按第 8 步恢复旧实例；第 10 步不分配内存且因第 2 步预留不得返回容量错误，若内部不变量破坏则进入 FAILED_RESTART_REQUIRED，不在 RAM 中回退已提交 generation
12. 按 4.11 从 PAUSED 关闭旧实例；若旧 strand handler 无法返回，记录 FAILED_RESTART_REQUIRED 并受控重启
13. 热更新不承诺插件 TCP/UDP 会话不断线；系统网络服务保持运行，旧连接关闭，候选按自身策略重连
14. 重启后 registry journal 必须能区分提交前、提交后和激活失败，并选择同 security_epoch 内上一已知良好包

#### 4.10.7 可行性限制

PoC-A 必须固定并验证：ESP-IDF v6.0.2 patch/tag、Xtensa 编译器版本、ELF loader commit、C++26 产物、支持的 relocation、函数指针、.init_array、静态构造/析构、cache 同步、IRAM 调用和至少 1000 次加载/卸载。未通过前，本节只能作为目标设计。

---

### 4.11 插件卸载流程

核心原则：带外关闭入口并取消异步源，再通过实例 strand 有序执行生命周期入口。LifecycleManager 不直接调用插件代码。

#### 4.11.1 生命周期状态机

状态 说明
STAGED 已保存并验证容器，尚未执行插件代码
LOADING 正在分配段、重定位和构造受限上下文
PREPARED plugin_prepare 已成功，仅持有受限 prepare_context_t，尚未接收普通输入
ACTIVE 可接收事件、服务调用和显示命令
PAUSING 热更新 ingress gate 已安装，正在停止受管任务、结算旧异步操作并形成快照
PAUSED 热更新一致快照已形成；可执行 RESUME 恢复旧实例，或在提交后进入 QUIESCING
QUIESCING 已停止新用户 post 和受管任务，正在取消异步源并等待 cancellation fence
UNLOADING 正在 strand 调用 plugin_unload 并释放资源
UNLOADED 不再保留可执行代码和实例资源
FAILED_RESTART_REQUIRED 无法安全释放，必须整机重启

允许的主转换为 STAGED → LOADING → PREPARED → ACTIVE → QUIESCING → UNLOADING → UNLOADED。ACTIVATE 在 PREPARED 内使用 staging 服务视图执行，只有 durable commit 和 ingress 切换成功才转换为 ACTIVE。热更新旧实例使用 ACTIVE → PAUSING → PAUSED → ACTIVE（中止）或 PAUSED → QUIESCING（提交）；候选使用 PREPARED → ACTIVE。任一含插件代码的状态若无法安全推进，可转为 FAILED_RESTART_REQUIRED。

#### 4.11.2 正常卸载流程

1. 原子地将 ACTIVE 转为 QUIESCING 并关闭用户 post；此后新用户投递返回 FRAME_ERR_PLUGIN_STOPPING。已 PAUSED 的热更新旧实例从 PAUSED 直接进入 QUIESCING，不重复安装 ingress gate
2. 通知全部额外受管任务停止并等待最多 1 秒；任务最终结果必须在退出确认前使用预留节点转移到实例 strand
3. 对 EventBus（含 RPC）、NetProxy、StorageService 和 steady_timer 执行 cancel_generation(instance, generation, shutdown_epoch)
4. 各异步源将最后一个已预留 completion 节点转移到 strand 后发送 source_quiesced_ack；每个操作由完成/取消 CAS 胜者决定保留原结果或返回 FRAME_ERR_CANCELLED
5. 收齐固定异步源集合的 ACK 后，将尚未开始的普通 POST 原位转为 CLEANUP，并在 FIFO 尾部插入预留 CANCELLATION_FENCE。PAUSED 实例若从 PAUSE 后未重新开放任何 producer，可复用已完成的任务停止、source ACK 和 fence 证明并跳过第 2～5 步
6. fence 到达或复用证明成立后，始终向实例 strand 投递一次内部 QUIESCE operation；PAUSE 不替代 QUIESCE
7. quiesce completion 返回且 strand 只剩内部控制 operation 后，投递内部 UNLOAD operation
8. unload completion 返回后停止 strand，再释放 ResourceLease、MemoryService arena、IRAM 和 plugin_context_t
9. 将状态转为 UNLOADED，并提交注册表 journal

#### 4.11.3 无法停止时的处理

· 任一 strand handler 或受管任务未在时限内退出时，状态转为 FAILED_RESTART_REQUIRED
· 不调用 vTaskDelete、vTaskSuspend、堆强制回收或 plugin_unload
· 不释放可能仍被执行代码引用的内存和外设资源
· 按 4.9.2 写入故障记录并立即重启

---

### 4.12 FallbackUI 生命周期

核心设计：FallbackUI 是静态链接的 DisplayService 内置场景，仍由唯一 DisplayService Worker 操作 LVGL。

· 启动条件：启动中、系统恢复中，或没有通过验证且可激活的插件包
· 启动方式：DisplayService Worker 创建内置场景并显示启动、恢复或故障状态
· 停止时机：正常 UI 插件 generation 激活且首个显示事务提交成功
· 停止方式：在 DisplayService Worker 内原子切换场景根并延迟删除旧对象，不切换底层显示驱动指针
· 状态管理：作为核心场景管理，不伪装成可热更新插件

---

### 4.13 中断上下文内存分配

明确限制：中断服务程序中禁止使用 new/delete/publish_copy/publish_buffer。

替代方案：预分配固定块池，每次投递独占一个块，消费者处理完成后归还。

```c
#define ISR_BLOCK_COUNT 8
#define ISR_BLOCK_SIZE 2048

typedef struct {
    uint16_t len;
    uint8_t data[ISR_BLOCK_SIZE];
} isr_block_t;

void IRAM_ATTR isr_handler(void) {
    BaseType_t task_woken = pdFALSE;
    isr_block_t* block = isr_pool_try_acquire_from_isr();
    if (block == NULL) {
        increment_isr_drop_counter();
        return;
    }

    block->len = read_data_to_buffer(block->data, sizeof(block->data));
    if (xQueueSendFromISR(isr_queue, &block, &task_woken) != pdTRUE) {
        isr_pool_release_from_isr(block);
        increment_isr_drop_counter();
    }
    portYIELD_FROM_ISR(task_woken);
}
```

块池、队列、计数器更新和 ISR 中调用的函数必须位于 IRAM/DRAM，并通过目标 ESP-IDF 版本的 IRAM-safe 检查。

---

### 4.14 插件实例身份规范

· 不使用跨核全局 g_current_plugin_id，也不允许插件向核心服务提交裸 plugin_id
· 每个 plugin_context_t 和服务表由框架按 instance ID + generation 创建并绑定
· 内存、事件、资源、任务、网络和存储 opaque handle 均包含或关联该实例身份
· 框架受管任务入口可使用专用 FreeRTOS TLS slot 加速内部查找，但 TLS 不是授权来源
· ESP-IDF 配置必须显式预留除 pthread 保留槽以外的框架 TLS slot，并在任务退出时清理
· 中断上下文不继承任务身份，只能投递预先注册并绑定目标实例的固定块池索引

#### 4.14.1 ABI 公共头

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t frame_err_t;
typedef uint64_t plugin_instance_handle_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint64_t feature_bits;
} frame_abi_header_t;

typedef uint64_t steady_timer_handle_t;
typedef uint64_t managed_task_handle_t;

typedef struct managed_task_context managed_task_context_t;
typedef struct memory_service memory_service_t;
typedef struct event_bus event_bus_t;
typedef struct display_service display_service_t;
typedef struct storage_service storage_service_t;
typedef struct network_service network_service_t;
typedef struct log_service log_service_t;
typedef struct resource_lease resource_lease_t;
typedef struct prepare_context prepare_context_t;
typedef struct plugin_state_writer plugin_state_writer_t;

typedef struct {
    const uint8_t* data;
    size_t len;
    uint32_t schema_id;
    uint32_t schema_version;
    uint32_t crc32;
} plugin_state_view_t;

typedef struct {
    void (*invoke)(void* user);
    void (*destroy)(void* user);
    void* user;
} frame_post_operation_t;

typedef struct {
    void (*complete)(void* user, frame_err_t err);
    void (*destroy)(void* user);
    void* user;
} frame_completion_operation_t;

struct managed_task_context {
    frame_abi_header_t abi;
    bool (*stop_requested)(const managed_task_context_t* self);
    frame_err_t (*heartbeat)(const managed_task_context_t* self);
    frame_err_t (*post)(const managed_task_context_t* self,
                        frame_post_operation_t operation);
};

struct memory_service {
    frame_abi_header_t abi;
    frame_err_t (*alloc_image)(size_t size, size_t alignment, void** out_ptr);
    frame_err_t (*alloc_packet)(size_t size, size_t alignment, void** out_ptr);
    frame_err_t (*alloc_generic)(size_t size, size_t alignment, void** out_ptr);
    frame_err_t (*release)(void* ptr);
};

typedef struct {
    uint32_t command_type;
    uint32_t flags;
    const void* payload;
    size_t payload_size;
} display_command_t;

struct display_service {
    frame_abi_header_t abi;
    frame_err_t (*submit_copy)(const display_command_t* command);
};

typedef enum {
    FRAME_LOG_DEBUG,
    FRAME_LOG_INFO,
    FRAME_LOG_WARN,
    FRAME_LOG_ERROR
} frame_log_level_t;

struct log_service {
    frame_abi_header_t abi;
    frame_err_t (*write_copy)(frame_log_level_t level, const char* tag,
                              const void* payload, size_t payload_size);
};

struct prepare_context {
    frame_abi_header_t abi;
    const memory_service_t* mem;
    const log_service_t* log;
    frame_err_t (*report_self_test)(uint32_t test_id, frame_err_t result,
                                    const void* detail, size_t detail_size);
};

struct plugin_state_writer {
    frame_abi_header_t abi;
    frame_err_t (*append)(plugin_state_writer_t* self,
                          const void* data, size_t len);
    size_t (*remaining)(const plugin_state_writer_t* self);
};

typedef struct {
    uint8_t core_affinity;
    uint8_t priority;
    uint16_t reserved;
    uint32_t stack_size;
} managed_task_config_t;

typedef void (*managed_task_entry_t)(void* immutable_input,
                                     const managed_task_context_t* task_ctx);

typedef struct {
    frame_abi_header_t abi;
    frame_err_t (*post)(frame_post_operation_t operation);
    frame_err_t (*timer_create)(steady_timer_handle_t* out_timer);
    frame_err_t (*timer_expires_after)(steady_timer_handle_t timer,
                                       uint64_t duration_us);
    frame_err_t (*timer_async_wait)(steady_timer_handle_t timer,
                                    frame_completion_operation_t completion);
    frame_err_t (*timer_cancel)(steady_timer_handle_t timer);
    frame_err_t (*timer_destroy)(steady_timer_handle_t timer);
    frame_err_t (*managed_task_create)(const managed_task_config_t* config,
                                       managed_task_entry_t entry,
                                       void* immutable_input,
                                       managed_task_handle_t* out_task);
    frame_err_t (*managed_task_request_stop)(managed_task_handle_t task);
} scheduler_service_t;

typedef struct {
    frame_abi_header_t abi;
    plugin_instance_handle_t instance;
    uint32_t generation;
    uint32_t reserved;
    const scheduler_service_t* sched;
    const memory_service_t* mem;
    const event_bus_t* bus;
    const display_service_t* display;
    const storage_service_t* storage;
    const network_service_t* net;
    const log_service_t* log;
    const resource_lease_t* lease;
} plugin_context_t;
```

· 所有服务表首字段都必须是 frame_abi_header_t
· ABI major 不同直接拒绝；minor 向后兼容时只访问双方 struct_size 覆盖的字段
· feature_bits 中声明为 required 的未知能力导致加载失败，optional 能力可为空
· 所有 reserved 字段必须写零并在读取时忽略，为后续扩展保留
· scheduler_service_t 已绑定当前实例 strand，插件不能指定其他实例或裸 strand handle
· post 返回 FRAME_OK 后框架接管 operation，并在 invoke 返回后或 QUIESCING 丢弃时恰好调用一次 destroy
· post 失败时框架不调用 invoke/destroy，operation 所有权仍归调用者
· 异步服务返回 FRAME_OK 前必须预留 completion operation 节点；无法预留时同步失败且不得启动底层操作
· completion 被异步服务接管后必须恰好调用一次 complete，再恰好调用一次 destroy
· completion 与取消以 PENDING 到 COMPLETED/CANCELLED 的 CAS 线性化；先到者决定 err 与结果所有权
· QUIESCING 丢弃普通 post 时不在 LifecycleManager 或核心服务 Worker 调用 destroy，而是在原 strand 上执行 CLEANUP operation
· timer handle 固定属于创建它的实例和 strand；跨实例、旧 generation 或重复销毁返回 FRAME_ERR_INVALID_PTR
· managed_task_entry_t 只能访问 immutable_input 和 task_ctx，不得直接取得 strand 所拥有的实例可变状态；heartbeat 绑定当前任务，不能替其他任务喂狗
· submit_copy 和 write_copy 在返回 FRAME_OK 前复制其 payload；失败时不保留调用者指针

#### 4.14.2 插件生命周期入口

```c
typedef struct {
    frame_abi_header_t abi;
    frame_err_t (*prepare)(const prepare_context_t* ctx);
    frame_err_t (*prepare_cleanup)(void);
    frame_err_t (*import_state)(const plugin_state_view_t* state);
    frame_err_t (*activate)(const plugin_context_t* ctx);
    frame_err_t (*pause)(void);
    frame_err_t (*quiesce)(void);
    frame_err_t (*export_state)(plugin_state_writer_t* writer);
    frame_err_t (*resume)(const plugin_context_t* ctx);
    frame_err_t (*unload)(void);
} plugin_entry_table_t;
```

插件只导出一个固定名称的查询入口，用于返回 plugin_entry_table_t；不得依赖 C++ 名字修饰。pause 只允许 PAUSING → PAUSED，resume 只允许 PAUSED → ACTIVE，且失败恢复所需的受管任务和 I/O 注册由 resume 重建。每个入口的其他允许状态、可调用服务和幂等性由 4.10、4.11 定义。

· 有状态插件必须设置非零 state_schema_id、state_schema_version >= 1，并同时提供 export_state/import_state
· 无状态插件必须将两个 schema 字段写零且 export_state/import_state 为 NULL；热更新仍执行 PAUSE/RESUME 与 ingress 事务，只跳过状态序列化
· schema 声明与函数指针组合不合法时，loader 在调用 prepare 前返回 FRAME_ERR_STATE_SCHEMA

---

### 4.15 核心服务线程模型

服务 线程模型 说明
PluginScheduler 每核心一个共享 Worker 每实例 strand 严格 FIFO，实例间每次一个 operation 轮转
DisplayService 独立 Core 0 Worker 只执行核心显示命令和 LVGL
LifecycleManager 独立 Core 0 Worker 只协调状态并向 strand 投递内部控制 operation
MemoryService 线程安全 portMUX_TYPE 自旋锁
EventBus 核心路由状态 + 锁 事件和 RPC 结果 post 到关联 strand
LogGateway 独立 Core 1 Worker 串行消费有界日志队列；生产者只执行复制、入队和原子计数
StorageService 两个独立 Worker System 与 SD 分别串行，completion post 到关联 strand
NetProxy 独立 Core 1 Worker completion post 到关联 strand
UpdateManager 独立 Core 0 Worker 串行协调固件、插件和资源更新，不执行插件代码
ResourceLease 线程安全 自旋锁
WatchdogManager 核心监控 Worker plugin_io Worker 与额外受管任务分别监控
MetricsCollector 线程安全 原子操作

#### 4.15.1 generation 关闭 ACK 协议

· 每个插件 generation 的固定异步源集合恰好包含四个逻辑成员：EventBus（订阅事件与 RPC 合并）、NetProxy、StorageService 和 PluginScheduler steady_timer
· StorageService 只有在 SystemStorageWorker 与 SDStorageWorker 都停止该 generation 的 producer，并分别完成内部 channel ACK 后，才对 LifecycleManager 发送一个聚合 source_quiesced_ack
· DisplayService、LogGateway 和 ResourceLease 不向插件 strand 产生 completion，UpdateManager 不接受插件请求，因此不属于该集合；额外受管任务在发送 cancel_generation 前单独停止
· LifecycleManager 每次关闭分配单调 shutdown_epoch。每个固定源即使没有未决操作也必须为匹配的 instance、generation、shutdown_epoch 发送一次 ACK；重复、旧 generation 或旧 epoch ACK 被忽略
· ACK 的线性化点位于该源已禁止新的 generation producer，且最后一个 EVENT/COMPLETION/CLEANUP 节点已经转移到目标 strand FIFO 之后
· LifecycleManager 只有在四位 ACK bitmap 全部置位后才能插入 CANCELLATION_FENCE；任一源在关闭截止时间内未 ACK 时进入 FAILED_RESTART_REQUIRED，不释放实例资源

中断上下文限制：

· post_isr：✅ 仅允许固定块池索引（队列满则归还块并丢弃）
· publish_copy/publish_buffer：❌ 禁止
· MemoryService：❌ 禁止
· StorageService：❌ 禁止
· new/delete：❌ 禁止

---

### 4.16 生产信任链与反回滚

#### 4.16.1 固件信任根

· prod 固件必须启用 ESP32-S3 支持的 Secure Boot V2 和 Flash Encryption release mode
· bootloader、分区表和核心应用由 ESP-IDF 官方 Secure Boot V2 流程验证
· dev 构建不得烧写不可逆生产 eFuse，使用独立测试密钥和虚拟/测试流程
· 具体 Kconfig 名称必须按锁定的 ESP-IDF v6.0.2 tag 生成和核验，不沿用旧版本配置名

#### 4.16.2 插件签名密钥

· 插件使用独立的单一长期 ECDSA P-256 发布私钥，不复用固件 Secure Boot 私钥
· 验证公钥编译进受 Secure Boot 保护的核心固件
· 私钥不得进入仓库、设备镜像、普通 CI 日志或开发机默认配置；签名操作必须在受控离线签名环境执行
· 单钥泄露后不能仅靠插件更新撤销；唯一恢复路径是发布包含新公钥的核心固件。因此密钥泄露属于产品级事件
· .mpb 仍携带 key_id，当前只接受一个生产 key_id，为未来格式兼容保留字段

#### 4.16.3 security_epoch

· 设备最低允许 security_epoch 存入 eFuse 单调位，低于 floor 的插件包一律拒绝
· 同一 security_epoch 内允许回滚到上一已知良好 SemVer 和 generation
· 提升 epoch 必须经过签名包验证、供电稳定检查和显式发布审批后烧写 eFuse
· 文档和制造流程必须记录可用位数、编码方式、剩余提升次数和耗尽策略
· eFuse 单调位耗尽后，设备不能再提高硬件 epoch floor；固件或签名密钥更新不能恢复已耗尽位。制造阶段必须预留足够字段并定义寿命上限，耗尽设备停止接受需要新 epoch 的插件或按产品策略退役
· 插件激活不得自行提升 epoch；由核心 OTA/受信更新管理器统一执行

#### 4.16.4 加密分区

· /system、/plugins 和 /assets 对应内部 Flash 数据分区在 prod 中启用加密
· /sd 是可移除外部介质，内容不依赖介质保密性；所有 .mpb 在加载前仍必须验证签名和 payload hash
· Flash Encryption 只提供静态数据保密性，不替代包签名、长度校验和运行时实例隔离

---

### 4.17 受信更新管理器 (UpdateManager)

核心绑定: Core 0
任务优先级: 11

· 只接受本地受信控制面或已认证发布通道的更新请求，不向插件 ABI 暴露；同一时间最多执行一个更新事务
· 固件更新写入非活动 OTA 分区，使用 ESP-IDF image/签名校验并设置待验证启动；新固件通过启动自检后才确认，否则由 bootloader 回滚
· 插件包先下载到不可执行 staging，按附录 A 完整验证后写入 /plugins 的临时文件并 fsync，再原子 rename；运行中切换委托 4.10.6
· /assets 更新只接受签名资源 bundle，经 SystemStorageWorker 执行临时文件、fsync、journal 和原子 rename；不得原位覆盖当前读取中的文件
· 下载和 TLS 由核心网络通道完成，更新 Worker 不调用插件 callback；内部 Flash 文件写入仍由 SystemStorageWorker 串行化
· security_epoch 仅在新固件/包已验证、供电稳定、回滚兼容性检查通过且发布策略明确要求时提升；烧写后失败视为不可逆生产事件
· 更新事务、目标摘要、阶段和结果写入 /system/update journal；重启恢复必须能区分未提交 staging、已提交待验证和已确认版本
· OTA 写 Flash 对 cache、显示和网络时延的影响必须在 PoC-E 实测；未满足 SLA 时更新过程可显式进入维护模式，但不得伪称后台无扰动

---

## 5. 硬件资源调度表

资源 分配策略 限制
CPU Core 0 Watchdog (22) + Display (20) + Lifecycle (18) + Update (11) + SystemStorage (9) + SDStorage (8) + plugin_io_core0 (4) 插件 strand 共享 plugin_io_core0；OTA 写入时延必须实测
CPU Core 1 Net (12) + Log (10) + plugin_io_core1 (4) Wi-Fi/LwIP 亲和性与插件 Worker 竞争必须纳入 PoC-B
PSRAM (16MiB) generation arena + staging + LVGL 非 DMA 数据 + 网络/TLS 动态区 插件总配额 11.5MiB 仅在实测并建立专用 arena 后生效
内部 SRAM 核心静态段 + FreeRTOS 栈/TCB + 队列 + DMA + Wi-Fi/LwIP + LVGL DMA 缓冲 不使用芯片标称 512KiB 直接推导可用 heap
IRAM 核心关键路径 + ISR + .plugin_iram 插件 IRAM 预算以 linker map 和 loader section 实测为准
TF 卡 (FAT/exFAT) 插件、静态资源、日志导出 若不存在，使用 Fallback
内部 Flash /plugins 2MiB、/system 512KiB、/assets 3.25MiB prod 使用加密数据分区

每个可编码里程碑必须保存以下证据：

· linker map 与 idf.py size-components 输出
· 启动、稳态和峰值的各 heap capability free/minimum_free/largest_free_block
· 所有核心任务和代表性插件任务的栈高水位
· LVGL/SPI DMA 缓冲、Wi-Fi/LwIP、TLS、LittleFS/FatFs 的峰值占用
· 旧/新 generation 共存时的 PSRAM 与 IRAM 峰值

没有真实板级数据时，资源表中的数值只能标为目标上限，不能作为已验证容量。

### 5.1 每实例核心服务预算

资源 Manifest 字段 系统上限与耗尽行为
strand operation credit strand_queue_capacity、max_outstanding_operations 所有用户 POST/EVENT/COMPLETION 共用 credit；未决异步操作先占 credit，内部 CONTROL 节点在系统 Kconfig 中另行永久预留
timer max_timers 句柄池达到上限返回 FRAME_ERR_CAPACITY
受管任务 max_managed_tasks、managed_task_stack_size 创建前同时检查任务数、栈和全局内部 SRAM 预算
EventBus max_event_subscriptions、max_event_message_bytes、max_event_buffer_bytes、max_outstanding_rpc 订阅、单消息或 buffer 配额耗尽返回 FRAME_ERR_CAPACITY；单订阅投递无可用 credit 时按 4.3.2 丢弃并计数
DisplayService max_display_commands、max_display_bytes 实例命令 credit 或不可变 payload 预算耗尽时同步返回 FRAME_ERR_QUEUE_FULL 或 FRAME_ERR_CAPACITY
StorageService max_storage_requests、max_storage_io_bytes、max_storage_copy_bytes 请求数、单请求长度或 write copy 预算耗尽时不接管请求并返回 FRAME_ERR_CAPACITY
NetProxy max_tcp_connections、max_udp_sockets、max_network_requests、max_network_buffer_bytes、max_network_io_bytes max_network_buffer_bytes 同时覆盖已接管发送 copy 和按 max_len 预留的接收块；实例配额与系统池任一耗尽都返回 FRAME_ERR_CAPACITY
LogGateway max_log_records_per_second、max_log_buffer_bytes 超过速率时丢弃插件普通日志并计数；高严重度核心故障记录使用独立系统保留
ResourceLease max_resource_leases 租约配额耗尽返回 FRAME_ERR_LEASE_FULL
热更新 ingress backlog max_update_backlog_events、max_update_backlog_bytes 事务开始前一次性预留；提交前溢出中止更新并恢复旧实例
Loader staging 无实例字段 系统同时只允许一个候选 staging，文件大小受 CONFIG_FRAME_MAX_MPB_BYTES 限制；忙时返回 FRAME_ERR_BUSY

Loader 要求 `strand_queue_capacity > 0`，允许未使用的可选服务预算为 0；所有请求值都必须 `<= system_kconfig_limit`，并验证 `max_outstanding_operations <= strand_queue_capacity`。同一物理池还必须满足所有已加载 generation、候选 generation、核心保留和安全余量的总和；Manifest 配额通过不代表物理资源一定可分配。

---

## 6. 文件系统路径约定

路径前缀 文件系统 用途 示例
/system/ LittleFS (内部Flash) 配置、证书、journal、故障和降级日志 /system/session/journal.bin
/plugins/ LittleFS (内部Flash) Fallback 与已安装插件包 /plugins/fallback.mpb
/assets/ LittleFS (内部Flash) 内置图片、字体和只读资源 /assets/font/default.bin
/sd/ FATFS/exFAT (TF卡) 外部插件、资源和首选日志 /sd/plugins/wifi_scanner.mpb
/sd/logs/ FATFS 日志文件（首选） /sd/logs/app_2026-08-20.txt
/system/logs/ LittleFS 日志文件（降级） /system/logs/app_2026-08-20.bin

### 6.1 目标分区布局

标签 挂载点 Offset Size 用途
plugin_fs /plugins 0xA40000 0x200000 插件包
system_fs /system 0xC40000 0x080000 系统耐久数据
assets_fs /assets 0xCC0000 0x340000 静态资源

· system_fs 从当前 assets_fs 划出 512KiB，调整后的结束地址必须严格等于 0x1000000
· prod 的三个 LittleFS 数据分区都启用 encrypted flag，并由 Flash Encryption 透明处理
· /assets 默认只读挂载；更新资源时必须进入受信更新事务
· 插件私有数据必须路由到 `/system/plugins/{instance-name}/`，并由绑定实例的 StorageService 做路径授权
· 所有文档和代码统一使用 .mpb 自包含包；不得再从旁路 .elf 文件加载

---

## 7. 启动时序图

```text
app_main()
  │
    ├─ 1. Bootloader 信任链
    │     ├─ Secure Boot V2 验证 bootloader、分区表和应用
    │     └─ prod 确认 Flash Encryption release mode
    ├─ 2. 硬件与内存初始化
    │     ├─ 检查双核、PSRAM、内部 heap capability 和目标芯片
    │     └─ 初始化最小 MetricsCollector 与 RTC 故障提示读取
    ├─ 3. 内部 Flash 优先就绪
    │     ├─ 挂载 /system、/plugins、/assets
    │     ├─ 扫描 session、plugin_registry 和 faults journal，先确定故障 generation 隔离集合与上一已知良好包
    │     └─ 若 /system 不可用，设置 safe_mode_pending 并禁止插件加载；此阶段不调用尚未 READY 的 DisplayService
        ├─ 4. 创建核心服务 Worker
        │     ├─ DisplayService、LifecycleManager、UpdateManager、SystemStorageWorker、SDStorageWorker
        │     ├─ NetProxy、LogGateway 和 WatchdogManager
        │     └─ 每个关键服务使用独立 FreeRTOS 任务并报告 READY
        ├─ 5. 创建插件调度层
        │     ├─ Core 0 创建 plugin_io_core0 Worker
        │     ├─ Core 1 创建 plugin_io_core1 Worker
        │     ├─ 初始化有界 operation pool、ready-strand queue 和 timer 容器
        │     └─ 两个 Worker 注册 Task WDT 并报告 READY
        ├─ 6. 核心服务就绪
    │     ├─ 初始化 MemoryService、EventBus、DisplayService 和 ResourceLease
    │     ├─ DisplayService 报告 READY 后，若 safe_mode_pending 则显示内置 FallbackUI
    │     ├─ SDStorageWorker 尝试挂载 /sd；失败只标记 ABSENT/FAILED
    │     ├─ LogGateway 恢复未闭合会话并提交本次 BEGIN
    │     └─ NetProxy 启动连接并异步上报历史故障
        ├─ 7. 插件候选验证与准备
    │     ├─ 从已排除故障 generation 的 registry 视图中选择 /sd/plugins/*.mpb 或 /plugins/fallback.mpb 候选
    │     ├─ 完成容器、签名、epoch、ABI、依赖和内存预检
        │     ├─ 加载候选 generation 并创建绑定核心的实例 strand
        │     ├─ LifecycleManager 向 strand 投递 PREPARE operation
    │     └─ 候选摘要或 generation 命中隔离集合时拒绝执行并继续选择下一候选
        ├─ 8. 激活
        │     ├─ 首次启动以 staging 服务视图执行 ACTIVATE，成功后提交 generation、转为 ACTIVE 并释放服务意图
    │     ├─ 更新场景按 4.10.6 执行状态迁移和原子切换
    │     └─ 无可用插件时保持内置 FallbackUI
        └─ 9. 生命周期协调
            ├─ 主协调任务只等待核心事件和发起状态转换
            ├─ 核心服务 Worker 与 plugin_io Worker 已分别运行
            └─ 禁止主协调任务直接执行插件 handler 或永久 run_loop()
```

---

## 8. 错误码全集

错误码 值 说明
FRAME_OK 0 成功
FRAME_ERR_INVALID_PTR -1 无效指针
FRAME_ERR_TIMEOUT -2 操作超时
FRAME_ERR_QUOTA_EXCEEDED -3 内存配额超限
FRAME_ERR_LEASE_FULL -4 租约表已满
FRAME_ERR_PLUGIN_NOT_FOUND -5 插件未找到
FRAME_ERR_DEP_CYCLE -6 循环依赖
FRAME_ERR_ABI_MISMATCH -7 ABI 版本不匹配
FRAME_ERR_BUSY -8 资源忙
FRAME_ERR_INVALID_CONTEXT -9 当前线程不允许调用此 API
FRAME_ERR_NET_AGAIN -10 网络暂时不可用
FRAME_ERR_WDT_TRIGGERED -11 看门狗超时触发
FRAME_ERR_ROLLBACK_FAILED -12 热更新回滚失败
FRAME_ERR_QUEUE_FULL -13 队列已满，请求未被接管
FRAME_ERR_PLUGIN_STOPPING -14 插件实例正在静默或停止
FRAME_ERR_CANCELLED -15 异步请求已取消
FRAME_ERR_RESTART_REQUIRED -16 当前地址空间无法安全恢复，需要重启
FRAME_ERR_SIGNATURE_INVALID -17 包签名无效或 key_id 不受信
FRAME_ERR_PACKAGE_INVALID -18 包格式、长度、offset 或字段非法
FRAME_ERR_HASH_MISMATCH -19 payload 摘要不匹配
FRAME_ERR_FS_NOT_FOUND -20 文件或路径不存在
FRAME_ERR_FS_TIMEOUT -21 文件操作超时
FRAME_ERR_FS_IO -22 底层 I/O 错误
FRAME_ERR_FS_NO_TF -23 TF 卡不可用
FRAME_ERR_FS_RECOVERING -24 SD Worker 正在恢复
FRAME_ERR_DURABILITY_DEGRADED -25 耐久存储不可用，已降级为 best effort
FRAME_ERR_CAPACITY -26 配置配额、handle 池或预留容量不足，请求未被接管
FRAME_ERR_NOT_FOUND -27 通用对象或未决操作不存在
FRAME_ERR_INVALID_ARGUMENT -28 参数值、状态组合或数值范围非法
FRAME_ERR_MEM_FRAGMENTED -30 PSRAM 碎片化，无法分配所需连续块
FRAME_ERR_SEMVER_MISMATCH -31 语义化版本不满足依赖约束
FRAME_ERR_IRAM_EXHAUSTED -32 插件 IRAM 需求无法满足
FRAME_ERR_STATE_SCHEMA -33 热更新状态 schema 不兼容
FRAME_ERR_EPOCH_ROLLBACK -34 security_epoch 低于设备 floor
FRAME_ERR_TARGET_MISMATCH -35 插件目标芯片或工具链 ABI 不匹配

---

## 9. 附录 A：插件 Manifest 二进制格式 (.mpb)

`.mpb` 是自包含签名容器，不是旁路 ELF 的 Manifest。所有多字节整数使用 little-endian；解析器必须逐字段读取，禁止把未对齐输入直接强制转换为 C struct。

### 9.1 容器布局

区域 顺序 说明
fixed_header 1 固定长度头，包含所有区域 offset/length
manifest 2 有界 TLV 元数据
payload_table 3 ELF 与可选资源的描述和 SHA-256
payloads 4 至少一个 ELF payload，可包含资源
signature_record 5 ECDSA P-256 签名记录

### 9.2 固定头

字段 类型 说明
magic uint32 0x4D504246（"MPBF"）
format_major uint16 不兼容格式升级
format_minor uint16 向后兼容格式升级
header_size uint32 固定头实际字节数
total_size uint32 必须等于容器实际长度且 <= CONFIG_FRAME_MAX_MPB_BYTES
flags uint32 未知 required flag 必须拒绝
target_id uint32 固定值 FRAME_TARGET_ESP32S3
core_abi_major uint16 所需核心 ABI major
core_abi_minor uint16 所需最低核心 ABI minor
required_features uint64 必需能力位
optional_features uint64 可选能力位
manifest_offset uint32 manifest 起始位置
manifest_length uint32 manifest 长度
payload_table_offset uint32 payload table 起始位置
payload_count uint16 范围 1..16
payload_entry_size uint16 当前 entry 长度
signature_offset uint32 signature record 起始位置
signature_length uint32 signature record 长度
key_id uint32 当前生产版本只接受固化的单一 key_id
security_epoch uint32 不得低于 eFuse floor
reserved uint32[4] 写零，读取时忽略

### 9.3 Manifest TLV

每项由 field_id(uint16)、flags(uint16)、length(uint32)、value(length bytes) 组成。未知 required 字段必须拒绝，未知 optional 字段可以跳过。

字段 约束
name UTF-8，1..64 字节，设备内唯一
version 严格 SemVer `major.minor.patch`，最长 32 字节
requires 最多 32 项，每项最长 96 字节
max_memory_bytes 必须小于系统插件配额，并按 generation 单独计费
iram_required_bytes 必须等于 .plugin_iram section 实测值
core_affinity 仅允许 Core 0、Core 1 或框架选择
strand_queue_capacity 实例用户 POST、EVENT 和预留 COMPLETION 共用的 operation credit 总量，不含系统永久预留 CONTROL 节点
max_outstanding_operations 实例未完成 I/O、RPC 和 timer wait 上限，必须 <= strand_queue_capacity
max_timers 实例 steady_timer handle 上限
max_managed_tasks 范围 0..8
managed_task_priority 仅适用于额外受管任务，合法范围不得高于 plugin_io Worker 优先级 4
managed_task_stack_size 每个额外受管任务的字节数上限
heartbeat_interval_ms 合法范围由 Kconfig 定义，默认 1000ms
event_rate_limit 每秒事件上限
max_event_subscriptions EventBus 持久订阅上限
max_event_message_bytes 单个 EventBus publish 或 RPC payload 最大字节数
max_event_buffer_bytes EventBus opaque buffer 总字节上限
max_outstanding_rpc 未决 RPC 上限，且计入 max_outstanding_operations
max_display_commands 未决 DisplayService 命令上限
max_display_bytes DisplayService 不可变 payload 总字节上限
max_storage_requests 未决存储请求上限，且计入 max_outstanding_operations
max_storage_io_bytes 单个 StorageService read/write 请求最大字节数
max_storage_copy_bytes StorageService 已接管 write copy 总字节上限
max_tcp_connections TCP connection 与 listener 句柄合计上限
max_udp_sockets UDP socket 句柄上限
max_network_requests 未决网络请求上限，且计入 max_outstanding_operations
max_network_buffer_bytes NetProxy 已接管发送与接收 buffer 总字节上限
max_network_io_bytes 单次网络发送或接收的最大字节数
max_log_records_per_second 插件普通日志速率上限
max_log_buffer_bytes LogGateway 已接管插件日志 payload 总字节上限
max_resource_leases ResourceLease 句柄上限
max_update_backlog_events 热更新 ingress 延迟事件条目上限
max_update_backlog_bytes 热更新 ingress 延迟 payload 总字节上限
state_schema_id 16 字节 UUID；全零表示插件无状态且不支持状态迁移
state_schema_version uint32；无状态时必须为 0，有状态时必须 >= 1

### 9.4 Payload Entry

字段 类型 说明
payload_type uint16 ELF 或 RESOURCE
flags uint16 required、compressed 等；首版 ELF 不允许压缩
offset uint32 payload 起始位置
length uint32 存储长度
unpacked_length uint32 首版必须等于 length
alignment uint32 必须为 2 的幂并满足类型约束
sha256 uint8[32] payload 原始字节摘要
reserved uint32[2] 写零

必须且只能有一个 required ELF payload。所有 offset+length 计算使用防溢出的宽类型；任何越界、区域重叠、重复 required payload、非法对齐或尾随未描述数据都返回 FRAME_ERR_PACKAGE_INVALID。

### 9.5 签名记录与覆盖范围

字段 类型 说明
algorithm uint16 固定 FRAME_SIG_ECDSA_P256_SHA256
record_version uint16 固定 1
key_id uint32 必须与 fixed_header 一致
signature uint8[64] IEEE P1363 格式 r||s

签名输入为以下字节的 SHA-256：

1. fixed_header 的完整原始字节
2. manifest 的完整原始字节
3. payload_table 的完整原始字节

payload 内容通过已签名 payload_table 中的 sha256 绑定。验证顺序固定为：结构边界检查 → 签名验证 → 每个 payload hash 验证 → epoch/ABI/feature 检查 → ELF 解析。验证后必须从同一不可变 staging buffer 加载，禁止重新按路径打开文件，以消除 TOCTOU。

### 9.6 生成工具

`manifest_builder.py` 负责生成确定性 little-endian 容器、拒绝重复字段、从最终 ELF section table 提取 `.plugin_iram` 实际大小写入 iram_required_bytes、计算 payload hash 并输出待签名摘要。缺失/重复 section、提取失败或用户输入值不一致时拒绝生成包。生产私钥签名是独立离线步骤，工具不得从仓库默认路径读取私钥。构建必须保存 unsigned manifest、最终包 SHA-256、key_id、security_epoch 和工具版本作为发布证据。

---

## 10. 附录 B：插件开发规范

### 10.1 任务设计规范（强制性）

· 插件不得直接调用 xTaskCreate；所有任务通过框架受管任务 API 创建
· 每个受管任务持有 stop token 和 task token，每个循环和阻塞返回点都必须检查停止请求
· 任何可能阻塞的操作必须使用带有限超时的版本，禁止 portMAX_DELAY
· 每个受管任务独立喂狗，一个健康任务不能掩盖同插件中的卡死任务
· 收到停止信号后，必须在 1 秒内自行退出
· 禁止自行修改任务核心亲和性或优先级

### 10.2 自旋锁使用限制（强制性）

· portENTER_CRITICAL / portEXIT_CRITICAL 之间的代码执行时间 严禁超过 1ms
· 若需要长时间临界保护，必须使用互斥量（xSemaphoreTake / xSemaphoreGive）
· 临界区不得调用存储、网络、日志、显示、内存分配或任何可能阻塞的 API

### 10.3 内存分配规范

· 禁止使用标准库 malloc/free/new/delete（编译期拦截）
· 必须使用框架提供的 ctx->mem->alloc_image/alloc_packet/alloc_generic 或 C++ 便捷层
· 中断上下文中禁止使用 new/delete
· 不得缓存 event_view_t、StorageService callback data 或 prepare_context_t 中的临时指针
· opaque handle 只能交还给创建它的服务表和 generation

### 10.4 跨核心通信规范

· 跨核心通信必须经过事件总线
· 普通数据使用 publish_copy，大数据使用 buffer_alloc + publish_buffer
· 禁止在中断中调用 publish_copy/publish_buffer
· 插件不得直接访问另一个插件导出的函数、全局变量或 FreeRTOS 对象

### 10.5 显示规范

· 插件不得包含或调用 LVGL API，不得持有 lv_obj_t、lv_disp_t 或显示驱动指针
· UI 插件通过 ctx->display 提交有界、不可变 display_command_t
· 长时间布局、解码和业务计算在额外受管任务分片执行，结果 post 回实例 strand 后再提交显示命令
· DisplayService Worker 只接受类型化命令，不接受插件任意 callback
· 队列满时必须处理 FRAME_ERR_QUEUE_FULL，不得忙等重试

### 10.6 生命周期与异步回调

· prepare、activate、pause、resume、quiesce、export_state、import_state 和 unload 只能由实例 strand 串行调用
· prepare 只能使用 prepare_context_t 明确开放的能力，禁止创建外设租约、网络监听或持久订阅
· activate 返回成功前，插件必须已准备好接收事件；其服务调用处于 activation-staging，只形成已预留意图，不产生外部副作用或 callback
· durable commit 与 ingress 切换成功后，框架才把候选转为 ACTIVE 并释放 activation service intents；失败时丢弃意图并执行 PREPARE_CLEANUP
· QUIESCING 的关闭入口和 stop token 是带外原子状态，不依赖 strand handler 获得执行机会
· 未执行普通 POST 在原 strand 上原位转换为 CLEANUP 并调用 destroy；不得在 LifecycleManager 或核心服务 Worker 直接析构捕获
· 异步 completion 与取消使用 PENDING 到 COMPLETED/CANCELLED 的 CAS；完成先获胜则保留原结果，取消先获胜则在 strand 上返回 FRAME_ERR_CANCELLED，二者合计 exactly once
· unload 只在 cancellation fence、quiesce 和受管任务停止后由 strand 执行，必须幂等且不得阻塞

### 10.7 strand handler 规范

· 首版只允许 post，插件不得依赖 dispatch/defer 或内联调用语义
· 实例可变状态只能在 strand handler 和生命周期入口中访问
· 普通 handler 必须在 1ms 内合作式返回，不得无限循环、无限阻塞或等待另一个同 strand handler
· handler 不得尝试改变 strand 核心亲和性或 FreeRTOS Worker 优先级
· 额外受管任务只能处理隔离或不可变数据，结果必须 post 回 strand
· post 返回失败时调用者仍拥有 operation；返回 FRAME_OK 后不得再次访问或销毁其捕获

### 10.8 热更新状态

· export_state 只能写入框架提供的有界 writer，不得返回插件内部裸指针
· 状态包含 state_schema_id、state_schema_version、payload length 和 CRC
· import_state 必须拒绝未知 required schema；失败不得改变当前 ACTIVE generation
· pause 返回成功后不得再创建任务、rearm I/O 或修改快照状态；resume 必须从 PAUSED 重建已停止任务和已取消 I/O，失败则进入 FAILED_RESTART_REQUIRED
· 只有标记为 replayable 的不可变框架输入可进入延迟队列；插件不得假设 socket、文件句柄或异步 completion 跨 generation 迁移
· 状态迁移代码不得假设旧实例和候选实例共享 C++ 类型布局

---

## 11. 附录 C：编译与链接选项规范

### 11.1 核心固件

· 使用锁定 ESP-IDF v6.0.2 tag 自带的 Xtensa 工具链和 C++26
· CMake 配置阶段必须检查 IDF、编译器和目标芯片版本，不满足即失败
· dev/prod 使用独立生成 sdkconfig；仓库根部陈旧 sdkconfig 不得覆盖 preset defaults

核心默认使用：

```text
-fno-exceptions
-fno-rtti
-fno-unwind-tables
-ffunction-sections
-fdata-sections
```

### 11.2 动态插件

· 插件可使用 C 或受限 C++26，但与核心之间只允许附录 B 的 C ABI
· 默认隐藏全部符号，只导出一个固定名称的 `extern "C"` 查询入口
· 禁止直接链接未列入 loader import allowlist 的 ESP-IDF 或核心内部符号
· 全局静态构造、析构、动态 TLS、异常、RTTI 和 unwind 默认禁止；只有 PoC-A 验证并加入白名单后才能启用
· 插件构建必须可复现，并记录编译器版本、flags、linker script hash 和 ELF SHA-256

候选 flags：

```text
-fPIC
-fvisibility=hidden
-Wl,--gc-sections
```

`-fPIC`、链接器脚本、relocation 白名单和 cache 同步流程必须由选定 ELF loader 的 PoC-A 结果确认，不能仅凭编译成功视为可运行。插件 IRAM 代码必须进入 `.plugin_iram` 专用 section；构建工具从 ELF 计算 `iram_required_bytes` 并写入 Manifest。

### 11.3 禁止项检查

CI 必须拒绝：未定义导入不在 allowlist、导出符号超出查询入口、出现禁用 relocation、异常/unwind section、动态 TLS、直接 LVGL 引用、直接 xTaskCreate、标准 malloc/free/new/delete，以及 Manifest 预算与 ELF 实测不一致。

---

## 12. 附录 D：架构准入测试（PoC-A ~ PoC-E）

通用证据要求：每个 PoC 必须保存 ESP-IDF tag、编译器版本、ELF loader commit、板卡/屏幕版本、sdkconfig、linker map、固件与包 SHA-256、原始串口日志、指标原始样本、测试脚本和通过/失败结论。只有摘要没有原始证据视为未通过。

### PoC-A：动态 ELF 与工具链可行性

目标：证明 ESP-IDF v6.0.2、ESP32-S3、C++26、PSRAM/IRAM 动态 ELF 的组合可实现。

验证项 通过标准
工具链 核心 C++26 构建、链接并运行；插件只通过 C 查询入口交互
ELF 白名单 每种支持 relocation、section 和 import 都有正例；未知类型在执行代码前拒绝
PSRAM 代码 .text 在动态分配 PSRAM 地址执行正确，cache 同步和函数指针调用正确
IRAM 代码 .plugin_iram 可加载、调用、释放，预算与 iram_required_bytes 一致
C++ 特性 构造/析构、.init_array 等仅在验证后加入白名单；禁用特性有 CI 负例
生命周期 连续 1000 次加载/激活/静默/卸载，无崩溃、泄漏或 largest_free_block 单调恶化
热更新峰值 旧 ACTIVE 与候选 STAGED 同时存在时满足 PSRAM、IRAM 和栈预算
调度语义 多生产者严格 FIFO、同 strand 不并发、ready 队列无重复 strand、IDLE 竞争无丢唤醒
取消语义 普通 post 在原 strand CLEANUP 一次，I/O/RPC/timer completion 由完成/取消 CAS 胜者在当前启动周期 exactly once

任一关键项失败时，动态 ELF 架构停止门评审，不进入完整组件编码。

### PoC-B：显示实时性与 SPI 带宽

目标：在 300x400 RLCD/SPI 实机上验证 DisplayService 单一所有者和 UI SLA。

· 连续运行至少 10 分钟，采集不少于 10000 个帧周期和 10000 个高优命令延迟样本
· 高优命令成功入队到开始处理 p99 <= 1ms
· 显示任务帧周期 p99 <= 16.67ms；同时报告 p99.9、最大值和丢帧率
· 分别测试全帧 RGB565、25%/50%/100% 脏区，并记录 SPI 时钟、DMA 大小和有效吞吐
· 叠加 Wi-Fi 收发、TLS 握手、DURING 日志 4KiB/s、SD 读写和插件后台计算负载
· 创建至少 8 个持续有任务的插件 strand，验证每轮每 strand 一个 handler 的公平轮转和有界队列背压
· 若全帧 60fps 不满足带宽，则把产品承诺收敛为实测可达的最大脏区，不能用自动降帧掩盖失败

### PoC-C：插件故障、卸载与回滚

目标：验证同地址空间下可安全处理合作式退出和不可安全卸载。

· 注入 strand handler while(1)、有限超时等待失效、关闭中断、自旋锁超限、旧 generation 延迟回调和卸载竞态
· strand handler 卡死时，同核心其他插件 strand 也会停止获得执行机会；Display、Storage、Net 等独立核心 Worker 必须继续运行直到受控重启
· 未进入 FAILED_RESTART_REQUIRED 的合作插件在 1 秒内退出，所有异步回调 exactly once 完成，资源和 arena 只释放一次
· 非合作任务不得触发 vTaskDelete、内存释放或 plugin_unload；Task WDT 协调路径在核心服务健康时写 journal，Interrupt WDT 路径只写 RTC/panic 标记，随后受控重启
· FAILED_RESTART_REQUIRED 路径不执行或恢复 RAM callback，只验证 journal/RTC 故障记录和重启后 generation 隔离
· 重启后故障 generation 被隔离，并自动激活同 security_epoch 的上一已知良好包
· 候选 prepare/import/activate 任一阶段失败时，旧 ACTIVE generation 继续服务
· 注入 PAUSING 各阶段失败、延迟队列溢出和候选 activate 后提交失败，验证旧实例 resume、任务/I/O 重建及输入按到达序号无丢失回放
· 成功更新时只回放标记为 replayable 的框架输入；旧 generation completion 全部在旧 strand 结算，网络连接由候选重建
· 分别验证无状态到无状态跳过序列化、有状态同 schema 迁移，以及无状态/有状态混合或 schema ID 不同被拒绝
· 使用故障注入走查 STAGED 到 UNLOADED 的所有合法状态转换和非法转换拒绝

### PoC-D：SD 故障与存储隔离

目标：证明 SD 故障不会阻塞内部 Flash，且不会并发破坏驱动状态。

· 注入缺卡、运行中拔卡、CRC 错误、命令超时、队列满、恢复失败和底层调用不返回
· /system、/plugins、/assets 在所有 SD 故障场景继续响应并满足各自请求时限
· 未触发受控重启时，入队成功的每个请求 callback exactly once；入队失败的请求不调用 callback
· 底层调用不返回并触发受控重启时，不要求 RAM callback 跨重启执行；重启后验证文件系统与 journal 一致性
· 恢复严格由 SDStorageWorker 在当前调用返回后串行执行，日志证明不存在并发 deinit
· 不可返回的驱动调用由 WDT 触发受控重启，不宣称 GPTimer 可以取消 I/O
· SD 恢复后经过健康检查才进入 READY，恢复期间新请求返回 FRAME_ERR_FS_RECOVERING

### PoC-E：断电耐久与生产安全

目标：验证 journal、日志丢失上限、包解析、Secure Boot、Flash Encryption 和 epoch。

· 在 BEGIN/END 写入、fsync 前后、DURING 时间阈值和大小阈值各阶段随机断电，至少 1000 个循环
· FRAME_OK 的 BEGIN/END 在每次重启后都可恢复；未返回成功的操作允许不存在但不得形成伪闭合会话
· DURING 丢失不超过最后 1 秒或 4KiB，按先到阈值判断；超限即失败
· fuzz/篡改 magic、长度、offset、重叠、TLV、payload hash、签名、key_id、target、ABI 和 epoch，均在执行 ELF 前拒绝
· prod 实机证明 Secure Boot V2 与 Flash Encryption release mode 已生效，直接读取内部数据分区得到密文
· 低于 eFuse floor 的 epoch 被拒绝，同 epoch 的上一已知良好版本允许回滚
· 验证制造配置记录 epoch 位总量、预留策略和产品寿命上限；模拟耗尽后拒绝新 epoch，并确认固件/插件换钥不会虚假恢复单调位容量
· 单一长期插件私钥不出现在仓库、设备、构建日志和普通 CI 产物中

---

## 13. 编码顺序

序号 模块 说明
1 平台与 ELF PoC 锁定 ESP-IDF、工具链、loader、relocation、PSRAM/IRAM 和 C++26 能力
2 ABI、身份与错误码 定义公共 C 头、generation、opaque handle、状态机和 metrics
3 PluginScheduler 每核心一个 plugin_io_context、每实例 strand、有界池、FIFO轮转、timer、取消和 WDT 归因
4 WatchdogManager Task WDT 第一阶段通知、Interrupt WDT panic、故障归因和第二恢复截止时间
5 核心 Worker Display/Lifecycle/Update/SystemStorage/SDStorage/Net/Log 独立任务、READY 屏障和关联 strand completion
6 EventBus publish_copy、引用计数 buffer、RPC exactly-once 和 ISR 固定块池
7 MemoryService generation arena、配额、Program Header 预算和真实资源指标
8 ResourceLease 实例绑定资源、QUIESCING 拒绝和重启恢复
9 分区与 SystemStorage /system、/plugins、/assets、journal 与内部 Flash Worker
10 SDStorage /sd 独立 Worker、请求状态机、超时检测和串行恢复
11 LogGateway BEGIN/END 耐久确认、DURING 批量刷新和降级配额
12 NetProxy 网络句柄、buffer 所有权、单次 rearm、generation 取消和 TLS 资源预算
13 .mpb parser 与安全验证 结构、hash、ECDSA、target、ABI、epoch 和 fuzz 测试
14 PluginLoader与LifecycleManager prepare/activate/pause/resume、依赖、事务更新、卸载和故障隔离
15 UpdateManager与生产安全 固件 OTA、插件/资源事务、Secure Boot V2、Flash Encryption、核心换钥和 eFuse epoch 流程
16 示例插件与 host 测试 正常、版本不符、状态迁移、卡死和格式负例
17 PoC-A ~ PoC-E 全量准入测试 通过并归档证据后重新进行架构签署

---

### 13.1 本轮文档后续工程同步项

文件 后续修改
CMakeLists.txt 保留 C++26，增加 ESP-IDF v6.0.2、编译器与目标版本断言
CMakePresets.json 保持 dev/prod 独立 SDKCONFIG，避免根 sdkconfig 覆盖 defaults
sdkconfig.defaults 按 v6.0.2 核验双核、PSRAM、TLS slot、WDT、文件系统和诊断配置名
sdkconfig.defaults.prod 增加 Secure Boot V2、Flash Encryption release mode 和生产 WDT；不提交私钥
sdkconfig.defaults.dev 使用测试密钥和不烧不可逆生产 eFuse 的验证流程
partitions.csv 新增 system_fs@0xC40000/0x80000，assets_fs 调整到 0xCC0000/0x340000，并设置生产加密策略
components/app_framework 实现核心服务 Worker、双 plugin_io_context、实例 strand 和主生命周期协调器
各 service component 按本文档的 Worker、所有权、状态机和 exactly-once 契约实现

### 13.2 重新签署规则

· 当前状态保持 v4.1-draft，不得在未完成 PoC 时恢复“最终定稿”措辞
· PoC-A ~ PoC-E 必须全部通过，不接受用风险说明替代失败项
· 终审逐项检查路径、错误码、ABI、状态机、Manifest、启动/更新/卸载时序和资源预算的一致性
· 通过后发布新的已签署版本，不直接覆盖本草案的测试证据
