# 更新：将受安全边界约束的 PX4 AI 助手迁入 QGroundControl Analyze 页面

本文是论坛文章 [Building a Safety-Bounded AI Flight Diagnostic Assistant in QGroundControl](https://discuss.px4.io/t/building-a-safety-bounded-ai-flight-diagnostic-assistant-in-qgroundcontrol/49228) 的后续更新。

## 为什么调整方向

QGroundControl 维护者在评论中认为这个概念有意思，但指出它不适合放在 MAVLink Console 中。核心问题是产品与架构边界：MAVLink Console 本身只支持 PX4，而 QGroundControl 功能应放在合适的独立入口中；维护者建议为它建立新的 Analyze 页面。

本次更新落实了这项建议，同时不宣称尚未实现的多固件能力：

- 助手迁移到独立的 **AI Flight Diagnostics (PX4)** Analyze 页面。
- MAVLink Console 恢复为独立控制台工具。
- 固件专属诊断逻辑放入 Provider 边界。
- 第一阶段有意只注册 PX4 Provider。

它仍然是实验性、非官方分支，不是 QGroundControl 上游正式功能。

## 新 Analyze 页面

![AI 飞行诊断 Analyze 页面](assets/ai-assistant/ai-flight-diagnostics-analyze-zh.png)

新页面显示当前固件、解锁/飞行状态、链路状态和可用诊断数据；同时提供对话、提问、取消、清空、设置及工具审批卡片。

Console 证据不再直接来自 MAVLink Console 页面。默认折叠的附件框只接收用户主动粘贴的文本；页面明确提示文本会发送给所选服务，只发送末尾 12,000 个字符，并在清空会话或切换 Vehicle 时清除。

无 Vehicle 时，只有提供附件后才能进行只读分析，且所有本地工具禁用。连接非 PX4 Vehicle 时，会在发送任何 AI 请求前禁用诊断。

## 重构后的架构

![新版 QGroundControl AI 诊断数据流](assets/ai-assistant/ai-flight-diagnostics-data-flow.svg)

原控制器拆分为：

- **AIDiagnosticController**：面向 QML，负责对话、请求、取消、审批、服务和工具执行协调。
- **AIDiagnosticContextBuilder**：构建版本化 QGC 上下文，包括核心状态、传感器摘要、健康/解锁检查、链路、FactGroup、电池、PX4 证据和显式附件。
- **AIDiagnosticProvider**：定义固件诊断扩展边界。
- **PX4DiagnosticProvider**：负责 PX4 支持判断、外部视觉证据、参数和 Console 策略、问题自动路由、MAVLink 数据消息白名单与 PX4 Prompt 语义。

上下文从 schemaVersion 1 开始，后续数据变化可以作为明确的接口契约审查。

## 保留并加强安全边界

![PX4 Provider 与安全校验](assets/ai-assistant/ai-flight-diagnostics-provider-safety.svg)

助手仍不能解锁、上锁、移动、起飞、降落、切换模式、写入/重置参数、校准、重启、运行执行器测试、修改任务，也不能执行任意 Shell/MAVLink 命令。

OpenAI-compatible 路径只暴露固定的本地只读工具注册表。受限 PX4 Console 请求使用命令白名单和单问题次数限制；REQUEST_MESSAGE 与临时 SET_MESSAGE_INTERVAL 只能请求白名单遥测消息，并始终要求用户明确确认。

PX4 已解锁、正在飞行或通信丢失时，Vehicle 侧 Console/MAVLink 请求会被拒绝。请求绑定开始时的活动 Vehicle；切换、断开或销毁该 Vehicle 会取消请求并清除审批。

ChatGPT/Codex 路径仍然只发送上下文并接收回答，以只读方式启动，不能调用 QGroundControl 本地工具。

## 设置兼容

![AI Assistant 设置页](assets/ai-assistant/ai-assistant-settings.png)

设置类更名为 AIAssistantSettings，QML 使用 aiAssistantSettings，同时暂时保留 aiConsoleSettings 兼容别名。底层 QSettings 分组仍为 AIConsole，因此原型版本中的 Endpoint、模型、API Key、ChatGPT 模型选择和遗留字段会被保留。

## 验证范围

更新后的分支增加了以下自动化覆盖：

- 新 Analyze 页、AI 设置页和独立 MAVLink Console 页加载
- 版本化的仅附件上下文
- 旧设置存储与兼容别名
- PX4/非 PX4 Provider 支持边界
- 参数名、Console 命令和 MAVLink 消息白名单
- 外部视觉自动诊断路由与证据语义
- Codex App Server 客户端协议行为

最终分支同时执行 Debug 构建和中英文 VitePress 文档构建。PX4 SITL 手工验收继续覆盖状态、参数、外部视觉、审批、取消、车辆切换、已解锁/飞行、失联和仅附件场景。

## 范围与后续

第一阶段暂不考虑 ArduPilot。下一步更值得讨论的是 Analyze 页面体验、Provider 边界、上下文契约与安全规则是否合理，而不是在缺少同等诊断能力和测试时只增加一个固件标签。

后续方向包括：

1. 增加发送数据预览和脱敏辅助。
2. 在语义和测试明确的前提下增加 PX4 证据适配器。
3. 扩大 Linux/Windows SITL 覆盖。
4. 评估本地模型服务路径。
5. 对任何新增固件 Provider 单独提案。

欢迎继续反馈新的 Analyze 放置方式，以及 QGroundControl 数据、PX4 专属证据和 AI 服务行为之间的边界。
