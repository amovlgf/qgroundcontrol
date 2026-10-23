# QGroundControl 开发交接

更新日期：2026-10-23。本文描述当前 RID/UOM 分支的代码实现，供下一次 Codex 对话接续开发。
“已实现”表示代码中已有对应流程；真实硬件验收和本次检查结果单独记录，不据此宣称正式平台接入或认证完成。

## 仓库与分支

- 工作目录：`/home/amovlgf/QGroundControl`。
- 当前分支：`codex/rid-device-management`。
- 个人远程：`fork` → `git@github.com:amovlgf/qgroundcontrol.git`。
- 上游远程：`origin` → `https://github.com/mavlink/qgroundcontrol.git`。
- 本次提交推送目标为个人远程的同名分支；后续操作前重新确认分支和工作区状态。
- 既有基础提交：`fe698f1c1` 修复 RemoteID 高度基准和数据时效；`66da8e60f` 增加每机 RID 身份及 UOM 模拟上报。
- 本次功能提交：`feat(RID): add device management and activation synchronization`；以当前分支的 `git log -1` 查看最终提交号。

## 已实现功能

### RID 设备管理

- 车辆设置增加 RID 页面，RemoteID 工具栏提供跳转入口，并补充中文界面文本。
- 经飞控读取 CAN 节点并探测 RID，支持多个设备发现、选择和异常诊断。
- 展示产品编号 SN、硬件 UID、CAN 节点、平台激活状态和 RID 当前生效状态。
- 首次出厂绑定支持 20 字符可打印 ASCII SN 和 0–4 飞机类别，要求明确确认硬件 UID。
- 绑定前检查未绑定、未激活、未锁定和固件能力；保存后回读 SN、类别、身份版本。
- 支持从平台查询已有激活状态，写入目标 RID 后再次读回核实。
- 按设备返回的元数据创建 Facts，支持参数读取、类型及范围校验、修改和保存回读。
- 参数界面区分只读、密码遮蔽、需重启和当前配置未使用等属性。
- 支持 RID 重启和恢复普通参数默认值，随后重新发现设备、检查重启结果并刷新参数。
- 恢复默认时核实默认参数，同时检查出厂身份和激活状态保留。

### 传输与事务保护

- AMOV MAVLink 消息 61003/61004 提供列节点和最多 192 字节设备透明传输。
- 提供 QGC/RID 共享编解码定义、DroneCAN DSDL、事务摘要向量及协议兼容性测试。
- 请求按 Vehicle、连接会话、请求号、节点、硬件 UID 和启动标识关联，防止误用旧响应。
- 写操作只允许当前活动车辆未解锁、未运动且通信正常时执行。
- 切换车辆、断链、解锁或开始运动会终止当前流程；重试前需要确认设备状态。
- 设备回报 `SAVED` 且具有 `DURABLE` 标志后才进入回读验证，不将传输成功视为保存成功。
- 对写入结果不明确的操作查询事务状态；出厂绑定另有进程内待确认记录，避免直接重复写入。
- AMOV 设备管理请求不进入 MAVLink 日志，包括路由回显；此规则不等于所有响应都已脱敏。

### RID 身份与 UOM 基础能力

- 每架飞机独立维护身份快照、身份摘要、启动标识、激活状态和上报会话。
- UOM 状态机覆盖等待身份、等待有效数据、鉴权、上报、恢复和故障状态。
- 校验遥测和平台 ACK 的时效，按 1 Hz 周期上报，并向飞控发送每机 UOM 状态。
- `COM_RID_UOM` 启用时接入飞行前 readiness；飞行中失去上报时记录告警。
- 提供本机 HTTP 模拟服务及显式 bench 构建路径，用于状态机和通信测试。
- RemoteID 操作员高度采用 WGS84 椭球基准，保留原始 GNSS 时间；实时位置超过 1 秒即失效。
- RemoteID 动态 SYSTEM 信息按 5 Hz 更新。

## 重要实现边界

1. **正式 UOM 平台上报尚未接入。** 默认 `UnconfiguredUomProvider` 不会宣布正式就绪。
   仅 `QGC_UOM_BENCH_TEST` 构建且 `QGC_UOM_MOCK_URL` 指向允许的本机地址时启用模拟 Provider。
2. 激活查询地址为 `https://uom.amovlab.cn/api/v1/device/check-binding`，通过 POST 提交 SN。
   校验 `sn`、`bound`、`unique_product_code` 和字符串型 `activation_state`；只同步平台已有状态。
   不负责在平台注册、绑定或发起激活；未绑定、未知应答或网络异常时保留 RID 当前状态。
3. RID 管理能力目前由 PX4 FirmwarePlugin 开启，需要匹配的 PX4 桥接与 RID 固件。
   本仓库提供 QGC 端和共享协议，不能据此认定飞控/RID 端功能已部署。
4. 出厂身份永久保留、断电恢复和恢复默认的实际效果需要在匹配 RID 固件上验证。
   进程内待确认记录不代表跨 QGC 重启的持久化事务恢复。
5. 本清单不代表已完成真实硬件全流程验收，也不构成监管合规或认证结论。

## 代码入口

下表路径相对仓库根目录；优先从管理器状态机、平台应答解析和共享协议开始阅读。

| 入口 | 用途 |
| --- | --- |
| `src/Vehicle/RID/VehicleRidManager.*` | 设备发现、绑定、激活、参数和维护状态机 |
| `src/Vehicle/RID/RidTransportClient.*` | MAVLink 会话、请求关联和超时处理 |
| `src/Vehicle/RID/RidActivationProvider.*` | 平台查询和严格应答解析 |
| `src/Vehicle/RID/RidActivationSyncController.*` | 激活查询结果与当前车辆/身份关联 |
| `src/Vehicle/RID/RidParameterManager.*` | 动态 Fact、参数元数据和数值校验 |
| `src/Vehicle/VehicleSetup/RidManagement.qml` | RID 管理页面及确认弹窗 |
| `src/Vehicle/VehicleSetup/RidParameterDialog.qml` | 参数编辑对话框 |
| `src/Toolbar/RemoteIDIndicatorPage.qml` | 工具栏状态及管理页面跳转 |
| `src/Vehicle/UOM/` | 身份、遥测、Provider 和上报状态机 |
| `src/Vehicle/VehicleReadiness.*` | UOM 与飞行前状态联动 |
| `protocol/amov_rid_uom/` | AMOV MAVLink 定义、生成和兼容性测试 |
| `protocol/amov_device_bridge/` | 设备业务协议、DSDL、编解码测试和摘要向量 |
| `test/Vehicle/RID/` | 平台应答及 RID 管理流程测试 |
| `test/QmlUITests/RidManagementUITest.*` | 设置页和工具栏入口测试 |
| `test/Vehicle/UOM/`、`test/Vehicle/VehicleUomManagerTest.*` | UOM 状态机与车辆集成测试 |
| `tools/uom_mock_server.py`、`tools/amov_m4/` | 模拟上报服务与容器构建工具 |
| `tools/run-rid-management.sh` | RID 界面启动及无界面启动检查 |

## AI 诊断开发是独立分支

- 当前分支的 `src/` 没有 AI 诊断模块，不能将发布目录中的副本当作已合入本分支。
- `dist/ai-release/source/` 是已登记 Git worktree，关联 `codex/ai-release-v5.0.8.1-ai.2`。
- 该分支实现独立 Analyze AI Flight Diagnostics 页面、PX4 Provider、上下文构建、只读工具审批和 AI 设置。
- AI 方向还存在 Windows/Android 工作树与分支；继续 AI 开发前先检查 `git worktree list`。
- **不能整体删除 `dist/`**；其中既有构建/发布材料，也有独立源码工作树。

## 本机清理保留项

- `build/` 当前存放 Android SDK、NDK、Qt 和 GStreamer 工具链，不能按普通桌面编译目录整体删除。
- `.cache/rid-local/` 和 CPM 依赖缓存含本地修补或特殊依赖准备，不能假定删除后可直接在线重建。
- 清理依赖缓存前先盘点并保留本地修改；依赖修补需要单独考虑如何形成可复现构建输入。
- 发布包、截图、论坛附件和其他工作树不属于本次 RID 代码编译中间文件。
- 本次清除了 RID CMake/Ninja 工程、对象文件、静态库、独立测试工程及 moc/ccache 缓存，释放约 **6.49 GiB**。
- 保留 `build-rid-management/Debug/` 和 `build-rid-management/qml/`，现有启动脚本仍可使用。
- 清理时已有 RID 窗口运行，因此未关闭它；该进程仍使用此前版本，查看本次磁盘程序需自行重启界面。
- 构建配置、清理清单和测试报告备份在 `.cache/rid-handoff-20261023/`，仅在本机保留，不提交到远程。
- `.gitignore` 排除 `dist/`、论坛附件 ZIP 和旧 ArduPilot 参数子模块副本，防止误加入代码提交。

## 本次验证结果

- 本机已有容器内增量构建成功，Ninja 确认无待编译内容。
- 完整 `ctest --output-on-failure -L Unit`：374/374 通过。
- RID/UOM/RemoteID 相关 CTest：9/9 通过，包含管理器、平台 Provider 和 QML 界面测试。
- QGC/PX4 双向 MAVLink 兼容性：5/5 通过，包含协议镜像一致性检查。
- 设备桥接编解码与 PX4 镜像检查通过，同时启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。
- 启动脚本的无界面启动检查在清理前后均通过。
- `git diff --check`、ShellCheck、Shell 语法、车辆空指针规则和翻译宏检查通过。
- 63 条新增/修改中文消息的翻译占位符通过；6 个变更 QML 的 Qt 6.12 `qmllint --bare` 退出 0，有警告但无错误。
- **完整 lint 未完成**：本机无 `just`、`uv`、Ruff、pre-commit，clang-format 10 无法读取仓库当前配置。
  上述针对性检查不等同于 `just lint` 全量通过；后续需按仓库工具说明安装匹配版本后补跑。
- 以上为软件验证；本次没有执行真实 RID 硬件验收或访问正式激活平台。

## 构建重建

本机环境是容器 `amov-m4-qgc:qt612-gcc12`、GCC 12 和外置 Qt 6.12：
`/home/amovlgf/.local/amov-m4/Qt`。容器 Python/CMake 位于 `/opt/qgc-python/bin/`。
保留程序可运行，但已删除 CMake 工程；下次改代码必须先重新配置再编译。

以下命令按清理前配置整理，复用本机保留的依赖；本次未在清理后重新执行完整编译：

```bash
cd /home/amovlgf/QGroundControl
AMOV_QGC_BUILD=build-rid-management AMOV_BUILD_JOBS=4 \
  bash tools/amov_m4/build_qgc.sh \
  -DQGC_UOM_BENCH_TEST=OFF \
  -DCPM_earcut_hpp_SOURCE="$PWD/.cache/rid-local/earcut_hpp" \
  -DCPM_libosmium_SOURCE="$PWD/.cache/rid-local/libosmium" \
  -DCPM_mavlink_SOURCE="$PWD/.cache/rid-local/mavlink" \
  -DCPM_nanobench_SOURCE="$PWD/.cache/rid-local/nanobench" \
  -DCPM_protozero_SOURCE="$PWD/.cache/rid-local/protozero" \
  -DCPM_rapidcheck_SOURCE="$PWD/.cache/rid-local/rapidcheck" \
  -DCPM_valijson_SOURCE="$PWD/.cache/rid-local/valijson"
bash tools/run-rid-management.sh
```

`build_qgc.sh` 默认开启 UOM bench，上述显式 `OFF` 与本次 RID 构建一致。
GStreamer 视频功能在此容器构建中关闭，这不代表源码移除了视频功能。
在新机器上需先准备工具链和依赖；本机 valijson、libevents、geographiclib 的依赖修改尚未整理为独立可移植补丁。

重建后可使用现有容器包装脚本运行测试，独立状态目录可设在 `/tmp`：

```bash
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 \
  AMOV_QGC_STATE=/tmp/qgc-rid-tests \
  bash tools/amov_m4/container.sh ctest --test-dir build-rid-management \
  --output-on-failure --no-tests=error -R 'Rid|Uom|RemoteID' --parallel 2
```

完整单元测试将最后一行的筛选改为 `-L Unit --parallel 4`。
跨端测试 `protocol/amov_rid_uom/test_protocol.py` 的 `--pymavlink-root` 应指向**包含 pymavlink 包的父目录**，
例如 PX4 的 `src/modules/mavlink/mavlink`，以及 QGC 的 `.cache/rid-local/mavlink`。
误指向包自身目录会回退到已安装的旧生成器，并可能触发 XML schema 不兼容。

## 下次开发与硬件验收重点

1. 先读 `AGENTS.md`、完整 `CODING_STYLE.md` 及任务相关贡献、工具、测试和 CI 说明。
2. 核对 QGC、PX4、RID 三端协议文件一致，避免只更新一端的消息或业务结构。
3. 使用真实 SN 核对平台的激活、未激活、未绑定、异常和超时响应。
4. 验证多节点发现、错误设备选择、车辆切换、断链、重连和 RID 重启后的事务隔离。
5. 验证首次出厂绑定、保存期间掉电、QGC 重启后重读、重复绑定拒绝及身份保留。
6. 验证锁定参数、密码参数、范围边界、需重启参数和恢复默认后的实际效果。
7. 验证解锁/运动限制，以及上报丢失时飞行前阻止与飞行中告警的行为。
8. 正式 UOM 接入需先取得平台接口与凭据，新增正式 Provider 并与 Mock 路径明确区分。
9. 执行本次改动对应测试、应用构建与 lint；最终 Unit 检查按 CI 命令运行并记录真实结果。
