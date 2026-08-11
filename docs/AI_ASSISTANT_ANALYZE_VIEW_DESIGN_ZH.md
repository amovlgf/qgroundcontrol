# RFC：QGroundControl Analyze 页面中的 PX4 AI 飞行诊断

## 状态

- 阶段：第一阶段实现
- 产品属性：实验性、非官方
- 固件范围：仅 PX4
- 基线：QGroundControl v5.0.8 实验分支

本文档可整理为上游 Issue/RFC，但不表示该功能已被 QGroundControl 官方接受。

## 背景

原型把 AI 助手放在 MAVLink Console 旁。QGroundControl 维护者指出：MAVLink Console 本身只支持 PX4，而一般 QGC 功能应考虑更清晰的产品边界，并建议为该功能建立新的 Analyze 页面。

第一阶段落实了这项结构性建议：AI 诊断成为独立 Analyze 工具，MAVLink Console 恢复为独立 Shell 页面；代码通过 Provider 接口保留固件扩展边界，但本阶段有意只注册 PX4 实现。

## 目标

- 为 PX4 用户提供基于证据的统一诊断入口。
- 将 AI 服务协调与固件诊断证据解耦。
- 保留只读工具、命令/消息白名单、用户审批和调用次数限制。
- 明确展示发送数据范围，尤其是手动粘贴的 Console 输出。
- 在构建 Prompt、工具或网络请求前拒绝非 PX4 固件。
- 设置改名后保留旧实验版本的数据。
- 为未来单独设计的其他固件 Provider 留出接口。

## 非目标

- 第一阶段支持 ArduPilot
- 解锁、起飞、降落、移动或模式切换
- 参数写入或重置
- 校准、执行器测试、重启、任务/围栏/集结点修改
- 任意 Shell 或 MAVLink 命令
- 后台采集 Console 或共享 Shell 服务
- 自动修复故障，或替代飞手判断

## 用户界面

Analyze 菜单新增 **AI Flight Diagnostics (PX4)**，与原 MAVLink Console 并列。

页面包括：

- Experimental、Unofficial、PX4 only 标识
- 连接、固件、解锁/飞行、链路与数据可用性摘要
- 对话记录、问题输入、Cancel、Clear 与 Settings
- 低权限 MAVLink 数据请求的审批卡片
- 默认折叠、仅允许用户粘贴的 PX4 Console 证据框
- 发送前的数据边界提示

| 状态 | 是否可提问 | 本地工具 | 网络请求 |
| --- | --- | --- | --- |
| 已连接 PX4 | 服务已配置时允许 | 按安全策略 | 允许 |
| 已连接非 PX4 | 禁用 | 禁用 | 禁用 |
| 无飞行器、无附件 | 禁用 | 禁用 | 禁用 |
| 无飞行器、有附件 | 允许 | 禁用 | 允许 |

附件只发送末尾 12,000 个字符。清空对话或切换活动飞行器时，附件同步清除。

## 模块边界

![Analyze 诊断数据流](assets/ai-assistant/ai-flight-diagnostics-data-flow.svg)

### AIDiagnosticController

面向 QML，负责对话、请求生命周期、取消、审批、服务选择、网络请求及本地工具执行器。它不定义 PX4 诊断语义。

每次请求绑定开始时的活动 Vehicle。车辆切换、销毁或通信丢失时取消请求并清除审批；工具始终使用已绑定 Vehicle，不会在执行中重新选择其他活动飞行器。

取消操作还会使当前异步请求代次失效。ChatGPT 通知只有在线程 ID 和轮次 ID 均与当前请求匹配时才会被接收，避免已取消请求的迟到事件进入新会话。

### AIDiagnosticContextBuilder

构建发送给两种服务路径的版本化 JSON，包括核心状态、传感器摘要、健康/解锁检查、链路、FactGroup、电池、Provider 证据和主动附加的 Console 文本。

该模块不采集 Shell 数据，也不推断固件专属语义。

### AIDiagnosticProvider

定义固件扩展边界：判断 Vehicle 支持性、提供结构化诊断证据和固件专属 Prompt 规则。第一阶段不注册其他固件实现。

### PX4DiagnosticProvider

负责：

- PX4 Vehicle 支持判断
- 外部视觉证据构建
- 传感器名称归一化与 Console 查询映射
- 只读 Console 命令与参数名策略
- MAVLink 消息白名单
- 低权限 MAVLink 请求的解锁/飞行/链路校验
- 自然语言问题的自动诊断路由
- PX4 证据语义与 Prompt 规则

![PX4 Provider 安全架构](assets/ai-assistant/ai-flight-diagnostics-provider-safety.svg)

## 版本化上下文

第一版顶层字段如下：

| 字段 | 含义 |
| --- | --- |
| schemaVersion | 当前为 1 |
| source | QGroundControl AI Flight Diagnostics |
| timestampUtc | 快照生成时间 |
| activeVehicle | 是否有实时 Vehicle 数据 |
| firmware | 固件可用性、支持性、名称及 MAVLink 类型 |
| core | 身份、飞行状态、模式、就绪状态、位置和计数 |
| sysStatusSensorInfo | QGC 传感器摘要 |
| healthAndArmingCheckReport | 当前健康与解锁检查证据 |
| linkStatus | 通信丢失、包计数、数传状态与签名 |
| factGroups | QGC FactGroup |
| batteries | 电池 FactGroup |
| px4DiagnosticEvidence | PX4 Provider 证据 |
| consoleAttachment | 附件可用性、来源、截断状态和文本 |

缺失证据必须明确标记 unavailable 或 unknown，不能被推断成故障。

## 两种 AI 服务路径

### OpenAI-compatible Chat Completions

发送版本化上下文和结构化本地工具定义。模型只能提出工具请求；工具名称、参数、Vehicle 状态和命令内容均由 QGroundControl 本地校验，模型不能提供任意可执行命令。

### ChatGPT / Codex App Server

使用只读沙箱、禁止审批和禁止本地网络执行策略。QGroundControl 只发送上下文并接收文本回答，不暴露本地工具注册表。

服务名称、任务标题、Prompt 和 HTTP User-Agent 均已从旧 MAVLink Console AI 定位更新为 PX4 AI Flight Diagnostics。

## 工具与安全策略

| 工具类型 | 示例 | 审批 | Vehicle 状态限制 |
| --- | --- | --- | --- |
| 内存只读 | 状态、FactGroup、健康、链路、已加载参数 | 否 | 已绑定 PX4 |
| 受限 PX4 Console 读取 | sensors status、固定 topic listener、param show | 否 | PX4、已上锁、未飞行、链路正常 |
| 单次 MAVLink 数据请求 | 白名单消息的 REQUEST_MESSAGE | 是 | PX4、已上锁、未飞行、链路正常 |
| 临时遥测频率请求 | SET_MESSAGE_INTERVAL，最高 5 Hz/30 秒 | 是 | PX4、已上锁、未飞行、链路正常 |

每个问题最多一轮模型工具调用、三条 PX4 Console 查询和一条低权限 MAVLink 请求；临时频率到期后恢复默认。

工具注册表不包含参数写入、任意 COMMAND_LONG、模式、任务、校准、执行器或飞行控制能力。

## 隐私边界

- Console 证据只来自显式粘贴。
- 不存在后台采集器或共享 Console Controller。
- UI 在发送前提示数据范围并显示截断状态。
- API Key 继续保存在原本的本地 QSettings 分组。
- 两条服务路径只接收当前请求构建的数据与有限对话历史。
- 远端服务自身的数据保留和账号条款仍然适用。

## 设置迁移

C++/QML 接口从 AIConsoleSettings 改名为 AIAssistantSettings：

- 新属性：<code>aiAssistantSettings</code>
- 临时兼容别名：<code>aiConsoleSettings</code>
- 底层 QSettings 分组继续使用：<code>AIConsole</code>

因此旧实验版本的 Endpoint、模型、API Key、ChatGPT 模型和遗留 OAuth 字段不会因改名丢失。

## 失败处理

| 场景 | 行为 |
| --- | --- |
| 非 PX4 固件 | 在上下文、工具和网络请求前拒绝 |
| 无 Vehicle 且无附件 | 本地拒绝 |
| 已解锁、飞行中或失联 | 跳过/拒绝 Vehicle 侧查询 |
| Vehicle 切换、销毁或断开 | 取消请求并清除审批 |
| 工具不在注册表或参数错误 | 返回 rejected 结果 |
| 超出次数限制 | 拒绝额外调用 |
| 网络错误或超时 | 结束 busy 状态并显示错误 |
| 证据缺失 | 标记 unavailable/unknown，不猜测 |

## 测试矩阵

自动化测试覆盖：

- 独立 Analyze 页、AI 设置页和纯 MAVLink Console 页加载
- 旧设置分组与兼容别名
- 无 Vehicle 附件模式的版本化上下文
- PX4/非 PX4 Provider 支持边界
- 参数名、Console 命令和 MAVLink 消息白名单
- PX4 外部视觉自动路由
- 外部视觉输入、配置、融合、无效、未知和不可用语义
- Codex App Server 协议解析与客户端行为

手工验收应在 Linux/Windows PX4 SITL 覆盖状态、参数、外部视觉、审批、取消、车辆切换、已解锁/飞行、失联和仅附件流程。

## 后续方向

1. 收集维护者对 Analyze 导航、命名和 Provider 边界的反馈。
2. 只在数据语义与安全检查可测试时扩展 PX4 证据适配器。
3. 增加脱敏辅助和请求数据预览。
4. 将本地模型能力与诊断安全策略分开评估。
5. 其他固件 Provider 必须通过独立提案、实现责任人和测试矩阵推进。
