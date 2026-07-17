# wifibroadcast-ng notes

## 包依赖关系

| 包 | 链路自适应 | 协调启动 |
|---|---|---|
| `legacy/wifibroadcast` | 无独立自适应模块 | `select BR2_PACKAGE_DATALINK` |
| `wifibroadcast-ng` | `adaptive-link` / `alink_drone` | 不依赖 datalink |

## 链路自适应

- **wifibroadcast（legacy）**：`select BR2_PACKAGE_DATALINK`，依赖 `datalink` 包。
  - `S98datalink` 负责协调启动：首次运行 `tweaksys`，然后 `wifibroadcast start`
  - `datalink.conf` 控制 telemetry/tunnel 等开关
  - `tweaksys` 做首次系统初始化，会创建 `/etc/system.ok` 阻止后续重复初始化

- **wifibroadcast-ng**：不依赖 `datalink`。
  - 使用 `adaptive-link` / `alink_drone` 做链路自适应
  - `/etc/wfb.yaml` 的 `link_control: alink` 控制是否启用

## `/etc/system.ok` 机制分析

### 谁创建 `system.ok`

| 位置 | 触发条件 |
|---|---|
| `wifibroadcast-ng` 的 `video_settings()` | 首次 `wifibroadcast start` 且 `system.ok` 不存在 |
| `datalink` 的 `tweaksys` | `S98datalink start` 时 `system.ok` 不存在 |
| `rubyfpv` 的 `tweaksys` | `S73ruby start` 时 `system.ok` 不存在 |

### 谁检查 `system.ok`

| 位置 | 逻辑 |
|---|---|
| `wifibroadcast-ng` 的 `start()` | `system.ok` 不存在 → 执行 `video_settings()` 做首次初始化 |
| `S98datalink start` | `system.ok` 不存在 → 先跑 `tweaksys`，再 `wifibroadcast start` |
| `S73ruby start` | `system.ok` 不存在 → 先跑 `tweaksys`，再启动 ruby |

### `alink_drone` 的启动时机

`video_settings()` 中写入 `alink_drone &` 到 `/etc/rc.local`：

```sh
sed -i '/alink_drone &/d' /etc/rc.local && sed -i -e '$i alink_drone &' /etc/rc.local
```

**关键点**：这只是把 `alink_drone &` 写进 `rc.local`，并不是立刻启动。真正启动要等下次 `rc.local` 执行（通常是下次开机）。

### `sensorConfig` 的写入时机

`video_settings()` 还会根据当前传感器和 SoC family 写入 Majestic 的 ISP 配置路径：

```sh
sensor=$(ipcinfo -s)
family=$(ipcinfo -f)

cli -s .isp.sensorConfig /etc/sensors/"$sensor"_"$family".bin
```

因此它会在以下情况下修改 `/etc/majestic.yaml` 的 `.isp.sensorConfig`：

1. 首次 `wifibroadcast start` 且 `/etc/system.ok` 不存在。
2. 执行 `wifibroadcast reset`。

注意：如果 `ipcinfo -s` 返回空字符串，而 `ipcinfo -f` 返回 `infinity6e`，脚本会写入：

```yaml
sensorConfig: /etc/sensors/_infinity6e.bin
```

这个路径通常不存在。对于 IMX415 + infinity6e，期望路径通常是：

```yaml
sensorConfig: /etc/sensors/imx415_infinity6e.bin
```

遇到 `/etc/sensors/_infinity6e.bin` 时，应先检查 `ipcinfo -s` 是否为空，以及 `/etc/sensors/` 下实际安装的 sensor config 文件。

### 典型故障场景

#### 场景 1：`datalink` + `wifibroadcast-ng` 共存

1. 开机 `S98datalink` 先执行，发现 `system.ok` 不存在
2. 调用 `tweaksys "$chip"`，`tweaksys` 内部 `touch /etc/system.ok`
3. 然后 `S98datalink` 调用 `wifibroadcast start`
4. `wifibroadcast start` 发现 `system.ok` 已存在，跳过 `video_settings()`
5. 结果：`rc.local` 没有被写入 `alink_drone &`，alink 不会自动启动

**解决方案**：关闭 `BR2_PACKAGE_DATALINK`，只用 `wifibroadcast-ng`。

#### 场景 2：只启用 `wifibroadcast-ng`，但 firmware 升级后 `system.ok` 已存在

1. 旧固件已生成 `/etc/system.ok`
2. 新固件中 `wifibroadcast start` 发现 `system.ok` 存在
3. 跳过 `video_settings()`
4. 结果：即使 defconfig 改了新参数，`video_settings()` 里的初始化不会再次执行

**解决方案**：升级后如果希望重新初始化，删除 `/etc/system.ok`，重启。

#### 场景 3：设备只跑 `wifibroadcast start`，但没有开机重启

1. 设备上没有 `system.ok`
2. `wifibroadcast start` → `video_settings()` → 写入 `alink_drone &` 到 `rc.local`
3. 当前进程里并没有启动 `alink_drone`
4. 需要手动 `alink_drone &` 或重启

**注意**：`alink_drone` 是 adaptive-link 的摄像头/飞机端（drone）组件。地面端组件是 `alink_gs`，不在摄像头固件内。

#### 场景 4：`sensorConfig` 被写成 `/etc/sensors/_infinity6e.bin`

1. 设备上没有 `system.ok`，或执行了 `wifibroadcast reset`。
2. `wifibroadcast` 调用 `video_settings()`。
3. `ipcinfo -s` 返回空字符串。
4. `video_settings()` 写入 `/etc/sensors/_infinity6e.bin`。
5. Majestic 或其它读取 `.isp.sensorConfig` 的工具加载 sensor config 失败。

**解决方案**：修复 `ipcinfo -s` 的传感器识别，或手动将 `.isp.sensorConfig` 设置为实际存在的文件，例如：

```sh
cli -s .isp.sensorConfig /etc/sensors/imx415_infinity6e.bin
```
