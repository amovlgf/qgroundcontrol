# B950 直接对接 UOM

QGC 使用方式二直接连接民航局 UOM，完成实名状态核验、激活状态上报及注销登记。
操作入口位于载具配置的 RID 管理页面，也可从飞行界面的 Remote ID 状态面板进入。
本功能首先支持 Linux；它不接入公司平台，也不替代原有 RID 运行识别信息播报。

## 准备 B950 联调

1. 在 [UOM 官方网站](https://uom.caac.gov.cn/)申请生产者权限，为产品型号 **B950** 选择方式二开展接口联调。
2. 在该流程绑定样机产品唯一识别码 **ALABB950B950U4400001**。一个联调流程只绑定一台样机。
3. 取得本环境的生产者 ID、密钥 ID 和三个接口地址。方式二不强制填写固定 IP。
4. 在 RID 管理页读取设备，核对实际产品码与样机一致。QGC 的“预期飞机 SN”仅用于核对，不会写入或覆盖 RID 出厂身份。
5. 打开“配置 UOM 连接”，选择联调环境，填写生产者 ID、密钥 ID 和预期飞机 SN。

联调地址为：

| 操作 | 地址 |
| --- | --- |
| 实名状态核验 | `https://uom.caac.gov.cn/api/uom-uavreg/eqgb/openapi/verifyRegisterFactory` |
| 激活状态上报 | `https://uom.caac.gov.cn/api/uom-uavreg/eqgb/openapi/reportActiveStateFactory` |
| 注销登记 | `https://uom.caac.gov.cn/api/uom-uavreg/eqgb/openapi/cancelRegisterFactory` |

密钥不显示在页面中，不写入普通设置或日志。保存时留空表示保留所选环境已经保存的密钥。
Linux 使用系统 Secret Service（libsecret）密钥环。页面会显示当前密钥存储状态；如果系统密钥环不可用，只在本次运行中保留密钥，重新启动后需要重新输入。
没有有效凭据时不会发起官方接口请求。

## 状态与操作

页面分别显示三种结果及核验、上报时间，不可相互替代：

- **实名登记状态**：UOM 最近一次核验的正常、注销、无记录或未知。
- **设备激活状态**：已连接设备的实际状态；上报成功本身不代表设备已经激活。
- **最近 UOM 确认**：最近一次激活状态上报的结果；它不是官方激活状态查询接口。

实名登记由所有者在官方 UOM 页面完成，QGC 不收集身份证信息、不代做人脸识别。
完成后返回 QGC 核验。只有登记正常、身份一致且设备具备所需控制能力时，才允许执行激活。
激活流程先改变设备状态并取得确认，再上报 UOM。

取消激活时必须选择“退出使用”“所有权变更”或“其他原因”，填写原因并确认目标飞机。
为避免服务器长度解释差异，原因限制为不超过 200 个 UTF-8 字节，中文通常占 3 字节。
正常流程先确认设备未激活，再上报未激活状态取得令牌，随后申请注销登记。
未完成全部步骤时，页面保留待处理状态，不显示整项操作成功。
若飞机故障导致无法执行取消激活，所有者可按照 UOM 提供的流程在官方平台直接办理注销。

操作遇到断网、超时或重启时，使用“重试待处理操作”恢复；重试前会再次提示可能发送注销请求。
同参数请求遵守至少 60 秒的间隔，连续点击不会绕过限制。
有待处理操作时，环境和飞机 SN 锁定。可以修正生产者 ID、接口地址或重新输入密钥；修改连接配置需勾选确认，原操作类型、注销原因及设备身份保持不变，新旧配置均留存审计。
保存修正不会自动写设备。点击“继续”后，QGC 重新读取同一架飞机并核对 UOM 登记状态；旧配置的注销令牌不再使用，必要时重新获取令牌后完成原操作。

## 飞控与 RID 的现有限制

本次不修改飞控或 RID 固件业务逻辑。QGC 必须获得设备已有接口的明确状态确认，不能将本机按钮状态当作飞控禁止飞行的证明。
页面只读取并显示相关解锁检查参数状态，不自动修改 `COM_ARM_ODID`、`COM_RID_UOM`，也不绕过解锁检查。
若当前 RID 固件缺少直接设置激活、取消激活或可靠回读能力，设备操作将失败，QGC 不会将未经确认的设备状态作为成功结果上报。
此时仍可完成 QGC 配置与实名核验；需要另行实现设备激活接口，才能验收“未激活或取消激活后不具备飞行能力”的完整闭环。

产品码读取、出厂身份绑定和普通 RID 参数功能仍在同一页面的 RID 区域。
样机码仅适用于上述联调样机，其他 B950 飞机必须使用自己的产品唯一识别码。

页面的“最近上报确认”只显示实际收到的 STATE1／STATE2 成功回执，其时间不会被随后注销完成或重启恢复的时间覆盖。
如果官方核对表明已经注销，但本次未收到 STATE2 成功回执，则不会显示“未激活上报成功”；旧版本记录没有回执时间时显示“未记录”。

## 转入正式环境与验收

完成 B950 全部联调步骤、提交符合性声明并审核通过后，由单位管理员在 UOM 创建正式接入配置。
正式环境使用独立接口地址和密钥；必须填写平台实际提供的地址，不能把联调地址当作正式地址。
QGC 将联调与正式环境的凭据、核验结果和待处理操作分开保存。

验收应覆盖：身份不匹配、未登记、已注销、登记正常但设备尚未激活、激活设备确认失败、上报超时、注销中断、重启恢复，以及更换飞机时状态不串用。
完整设备验收还应验证首次未激活和取消激活后的禁止飞行、激活后的重启保持、固件更新后功能保持。
最近一次激活的审计记录保存在应用本地数据目录的 `uom-registration.sqlite`，留存至少一年；记录不含密钥或注销令牌。

依据：[GB 46761—2025《民用无人驾驶航空器实名登记和激活要求》](https://www.caac.gov.cn/XXGK/XXGK/BZGF/BZGF_GJBZ/202601/P020260120370062157303.pdf)，
以及《民用无人机实名登记和激活对接 UOM 说明（2026-05-28）》。实际联调和正式环境配置以 UOM 当前提供的信息为准。

## 本分支的软件验证记录

开发基线为 `codex/rid-device-management` 的 `ba55f7ec8`，功能分支为 `codex/b950-uom-direct`。
2026-10-27 在 Linux 构建容器 `amov-m4-qgc:qt612-gcc12` 中使用 Qt 6.12、GCC 12、OpenSSL Crypto 和 libsecret 完成以下验证。
构建目录为 `build-uom-direct`，关闭 `QGC_UOM_BENCH_TEST`；未发送真实 UOM 请求。

| 检查 | 实际结果 |
| --- | --- |
| 完整 Linux 构建 | 通过 |
| 应用启动检查 | `--simple-boot-test --allow-multiple` 通过 |
| RID、UOM、Remote ID、相关界面回归 | 10/10 个 CTest 测试通过 |
| 新增接口、加密、存储及业务流程测试 | 两个测试套件通过；覆盖中文原因、持久化限流、恢复、旧响应、配置修复与令牌失效 |
| 本次修改文件的 pre-commit | 全部通过，`git diff --check` 通过 |
| 最终全量 Unit 检查 | 374/375 通过；唯一失败为 `BluetoothWorkerTest` |
| 全仓 pre-commit | 未全部通过，存在既有格式、静态检查问题及部分工具环境限制 |
| UOM 官方联调 | 未执行，尚无官方联调凭据 |
| B950 整机验收 | 未执行，需实机确认持久化、解锁限制与正式飞行数据上报 |

蓝牙测试失败来自无蓝牙适配器时的 `qt.bluetooth.bluez` 警告超出既有严格日志白名单。
使用修改前已有的 `build-rid-management/Debug/QGroundControl` 对照，两个相同测试用例同样失败；本次未修改蓝牙代码或测试。
全仓检查的既有失败包括 C++/CMake/Markdown 格式、脚本权限、Pyright、拼写及部分 QML 重复 ID。
首次只读全仓检查另遇到缓存、Vale 资源和 import-linter 依赖限制；补齐运行环境后全仓 Ruff 通过，不能把这些工具限制当作源码缺陷。

可在仓库根目录复现构建和测试：

```bash
bash tools/amov_m4/container.sh cmake --build build-uom-direct --parallel 4
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 \
  AMOV_QGC_STATE=/tmp/qgc-uom-validation bash tools/amov_m4/container.sh \
  ctest --test-dir build-uom-direct --output-on-failure --no-tests=error \
  -R 'Rid|Uom|RemoteID' --parallel 2
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 \
  AMOV_QGC_STATE=/tmp/qgc-uom-validation bash tools/amov_m4/container.sh \
  ctest --test-dir build-uom-direct --output-on-failure --no-tests=error -L Unit --parallel 4
```

当前工作区的详细检查命令及日志保存在 `.cache/uom-verification/`。
构建验证用的 libsecret 开发包位于本地缓存；交付 Linux 安装包仍须包含对应运行库或声明系统依赖。
系统密钥环的真实读写和官方接口验收须在具备 Secret Service、联调凭据及 B950 实机的环境执行；已验证密钥环不可用时的内存降级路径。
