# Adaptive Link 工作原理

`adaptive-link` 是 OpenIPC FPV 场景下的链路自适应控制器。它不是编码器内部的自适应码率算法，而是根据地面端接收质量选择一组预定义 profile，并同时调整无线链路参数和 Majestic 视频参数。

仓库中的 Buildroot 包位于：

```text
general/package/adaptive-link/
```

上游源码固定提交在：

```text
https://github.com/OpenIPC/adaptive-link
3a831a75cb25df403374fa5104ea494c140695da
```

## 组件

`adaptive-link` 分为两端：

- `alink_gs`：运行在地面端，读取 wfb-ng 接收统计并计算链路质量分数。
- `alink_drone`：运行在摄像头/飞机端，接收链路质量分数并切换 profile。

Buildroot 当前安装到目标系统的主要文件：

```text
/usr/bin/alink_drone
/etc/alink.conf
/etc/wlan_adapters.yaml
/etc/txprofiles.conf
```

## 控制链路

整体流程：

```text
地面端 wfb-ng RX stats
  -> alink_gs 读取 RSSI/SNR/FEC/lost packets
  -> 归一化为 1000~2000 的链路分数
  -> UDP 发送给飞机端 alink_drone
  -> alink_drone 平滑、迟滞、防抖
  -> 从 /etc/txprofiles.conf 选择 profile
  -> 调整 WFB MCS/FEC/带宽/发射功率
  -> 调整 Majestic video0.bitrate/gopSize/qpDelta
```

核心目标是让视频码率和无线链路能力同步变化：

- 近距离强信号：较高 MCS、较高码率、较好画质。
- 中距离：中等 MCS、中等码率。
- 远距离弱信号：低 MCS、低码率、更保守 FEC。
- 地面端心跳丢失：进入 fallback 最保守档。

## 地面端评分

`alink_gs` 连接 wfb-ng JSON stats，默认配置类似：

```ini
[json]
HOST = 127.0.0.1
PORT = 8103

[weights]
snr_weight = 0.5
rssi_weight = 0.5

[ranges]
SNR_MIN = 10
SNR_MAX = 36
RSSI_MIN = -85
RSSI_MAX = -40
```

地面端会取最佳 RSSI 和最佳 SNR，并映射成 `1000~2000` 分数：

```text
snr_normalized  = (best_snr  - SNR_MIN)  / (SNR_MAX  - SNR_MIN)
rssi_normalized = (best_rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN)
score_normalized = snr_weight * snr_normalized + rssi_weight * rssi_normalized
raw_score = 1000 + score_normalized * 1000
```

`1000` 表示链路差，`2000` 表示链路好。

如果启用噪声惩罚，`alink_gs` 还会结合 `lost packets`、`fec_rec packets`、FEC k/n 和 Kalman filter 后的 error ratio 扣减分数。

## UDP 消息

地面端发给飞机端的 UDP payload 前 4 字节是网络字节序 `uint32_t` 长度，后面是冒号分隔文本。

普通消息格式：

```text
timestamp:rssi_score:snr_score:fec_rec:lost:rssi:snr:num_antennas:penalty:fec_change[:idr_code]
```

主要字段：

- `rssi_score`：RSSI 评分，范围通常是 `1000~2000`。
- `snr_score`：SNR 评分，范围通常是 `1000~2000`。
- `fec_rec`：FEC 恢复包数量。
- `lost`：丢包数量。
- `penalty`：噪声惩罚值，用于 OSD 显示。
- `fec_change`：动态 FEC 调整等级。
- `idr_code`：可选关键帧请求码。

飞机端也支持特殊消息：

```text
special:request_keyframe:<code>
special:pause_adaptive
special:resume_adaptive
```

## Profile 配置

飞机端 profile 文件：

```text
/etc/txprofiles.conf
```

配置格式：

```text
# <range> <gi> <mcs> <fecK> <fecN> <bitrate> <gop> <Pwr> <roiQP> <bandwidth> <qpDelta>
```

示例：

```text
999  -  999  long 0 2 3    1000 10 30   0,0,0,0 20 -12
1000 - 1050  long 0 2 3    2000 10 30   0,0,0,0 20 -12
1051 - 1100  long 1 2 3    4000 10 30   0,0,0,0 20 -12
1101 - 1200  long 2 4 6    7000 10 30 12,8,8,12 20 -12
1201 - 1300  long 3 6 9   10000 10 30   2,1,1,2 20 -12
```

当链路分数落入某个 range 时，`alink_drone` 选择对应 profile。以上 `1051 - 1100` 代表：

```text
GI: long
MCS: 1
FEC: 2/3
video0.bitrate: 4000 kbit/s
video0.gopSize: 10
TX power: 30
bandwidth: 20 MHz
video0.qpDelta: -12
```

所以 `adaptive-link` 是分段切档，不是逐 kbit/s 连续调节。

## 飞机端选择逻辑

`alink_drone` 的主要处理链路：

```text
process_message()
  -> start_selection()
  -> value_chooses_profile()
  -> apply_profile()
```

`start_selection()` 先按权重合成分数：

```text
combined = rssi_score * rssi_weight + snr_score * snr_weight
```

然后做指数平滑：

```text
smoothed = factor * combined + (1 - factor) * previous_smoothed
```

升档和降档可以使用不同平滑参数：

```ini
exp_smoothing_factor=0.1
exp_smoothing_factor_down=1.0
```

接着做迟滞判断，避免 RSSI/SNR 抖动导致频繁切档：

```ini
hysteresis_percent=5
hysteresis_percent_down=5
```

只有变化百分比超过阈值，才会进入 profile 选择。

## 防抖和 fallback

`alink_drone` 使用这些配置限制频繁切换：

```ini
fallback_ms=1000
hold_fallback_mode_s=1
min_between_changes_ms=200
hold_modes_down_s=3
```

含义：

- `fallback_ms`：超过该时间没收到地面端消息，进入 fallback。
- `hold_fallback_mode_s`：进入 fallback 后至少保持多少秒。
- `min_between_changes_ms`：两次 profile 变化之间的最小间隔。
- `hold_modes_down_s`：从低速档升到高速档前的等待时间。

实际调整周期由地面端消息和这些限制共同决定：

- `alink_gs` 通常约每 `100ms` 发送一次链路统计，`alink_drone` 收到消息后才评估是否切换 profile。
- `min_between_changes_ms=200` 表示即使链路分数快速变化，profile 实际切换也至少间隔 `200ms`。
- `hold_modes_down_s=3` 会限制从低速档升到高速档，升档至少等待 `3s`。
- `fallback_ms=1000` 只在已经收到过地面端消息后生效；之后如果 `1s` 没有新消息，会尝试进入 fallback profile。

如果地面端心跳丢失，`alink_drone` 会触发：

```text
start_selection(999, 1000, 0)
```

这会选择 `999 - 999` fallback profile，通常是最低 MCS、低码率、保守 FEC。

注意：`fallback_ms` 不等于开机后立刻应用 fallback。当前源码会等待第一条地面端 heartbeat，之后才认为链路已初始化并启用丢消息 fallback。没有收到过 GS 消息时，`txprofiles.conf` 不会主动应用。

## 如何调整 Majestic 码率

`adaptive-link` 默认不是通过 `cli -s` 改配置后重启 Majestic，而是通过正在运行的 Majestic HTTP API 热更新参数。

`/etc/alink.conf` 中的默认命令模板：

```ini
bitrateCommandTemplate="curl -s 'http://localhost/api/v1/set?video0.bitrate={bitrate}'"
gopCommandTemplate="curl -s 'http://localhost/api/v1/set?video0.gopSize={gop}'"
qpDeltaCommandTemplate="curl localhost/api/v1/set?video0.qpDelta={qpDelta}"
roiCommandTemplate="curl -s 'http://localhost/api/v1/set?fpv.roiQp={roiQp}'"
idrCommandTemplate="curl localhost/request/idr"
```

例如切到 8000 kbit/s 时执行的是：

```sh
curl -s 'http://localhost/api/v1/set?video0.bitrate=8000'
```

这条路径是：

```text
alink_drone
  -> Majestic /api/v1/set
  -> Majestic 运行时更新编码参数
  -> 平台 HAL / VENC 驱动
  -> 硬件编码器码率变化
```

因此，`adaptive-link` 的设计目标是动态调整码率，不需要每次重启 Majestic。

需要区分：

- `cli -s .video0.bitrate 8000`：主要是修改配置存储，是否立即生效取决于是否通知 Majestic reload。
- `curl localhost/api/v1/set?video0.bitrate=8000`：直接调用运行中的 Majestic API，适合动态调节。

当前 Majestic 二进制中可以看到这些运行时接口字符串：

```text
/api/v1/set
/api/v1/set?
/request/idr
reload_sdk
Signal HUP received, reloading config
```

通常更适合热更新的参数：

- `video0.bitrate`
- `video0.qpDelta`
- `fpv.roiQp`
- `/request/idr`

可能需要重建 pipeline 或 reload SDK 的参数：

- `video0.codec`
- `video0.size`
- 部分传感器/ISP 参数
- 某些平台上的 FPS 变更

`adaptive-link` 默认修改 FPS 不是走 Majestic API，而是写 SigmaStar sensor proc 节点：

```ini
fpsCommandTemplate="echo 'setfps 0 {fps}' > /proc/mi_modules/mi_sensor/mi_sensor0"
```

## 无线参数调整

`adaptive-link` 同时通过 `wfb_tx_cmd` 调整无线链路：

```ini
mcsCommandTemplate="wfb_tx_cmd 8000 set_radio -B {bandwidth} -G {gi} -S {stbc} -L {ldpc} -M {mcs}"
fecCommandTemplate="wfb_tx_cmd 8000 set_fec -k {fecK} -n {fecN}"
powerCommandTemplate="iw dev wlan0 set txpower fixed {power}"
```

也就是说，profile 切换会同时影响：

- MCS
- GI long/short
- bandwidth
- LDPC/STBC
- FEC k/n
- TX power
- video bitrate
- GOP
- QP delta
- ROI QP

启动阶段需要区分两套功率来源：

- `wifibroadcast` 启动接口时读取 `/etc/wfb.yaml` 的 `.wireless.txpower`，并立即设置一次无线功率。
- `adaptive-link` 的 `/etc/txprofiles.conf` 中 `Pwr` 只有在 `alink_drone` 收到 GS 消息并应用 profile 后才生效。

因此，没有 GS heartbeat 时，开机默认功率通常保持为 `/etc/wfb.yaml` 的 `txpower`；不要把它误认为 `txprofiles.conf` 的 `Pwr` 解析失败。

## 升档和降档顺序

`apply_profile()` 会根据升档或降档使用不同命令顺序。

升档时，先提高链路能力，再提高视频负载：

```text
1. qpDelta
2. fps
3. tx power
4. gop
5. mcs/radio
6. fec + bitrate
7. roi
8. idr
```

降档时，先降低视频负载，再调整无线参数：

```text
1. qpDelta
2. fps
3. fec + bitrate
4. gop
5. mcs/radio
6. tx power
7. roi
8. idr
```

`manage_fec_and_bitrate()` 内部还有一次顺序控制：

- 如果新 bitrate 高于旧 bitrate：先改 FEC，再提高 bitrate。
- 如果新 bitrate 低于旧 bitrate：先降低 bitrate，再改 FEC。

这样可以减少切档瞬间把链路打爆的概率。

## TX dropped 保护

`alink_drone` 会周期读取：

```text
/sys/class/net/wlan0/statistics/tx_dropped
```

相关配置：

```ini
allow_xtx_reduce_bitrate=1
xtx_reduce_bitrate_factor=0.8
check_xtx_period_ms=2250
allow_rq_kf_by_tx_d=1
request_keyframe_interval_ms=1112
```

如果本机 WiFi 发送 drop 增加，`adaptive-link` 可以临时降低当前码率：

```text
new_bitrate = current_bitrate * xtx_reduce_bitrate_factor
```

例如：

```text
8000 kbit/s -> 6400 kbit/s
```

如果随后一段时间没有新的 TX dropped，再恢复原 profile 码率。

TX dropped 也可以触发 IDR 请求，帮助解码端在链路扰动后快速恢复画面。

## Keyframe 请求

`adaptive-link` 支持请求 Majestic/VENC 产生新的 keyframe/IDR。这个机制用于让解码端在丢包或参考帧污染后尽快恢复画面，不是视频包 ACK，也不是重传旧包。

### 地面端触发

地面端 `alink_gs` 会读取 wfb 接收统计。如果发现 `lost_packets > 0` 且 `alink_gs.conf` 允许 IDR 请求：

```ini
[keyframe]
allow_idr = True
idr_max_messages = 20
```

则生成一个随机请求码：

```text
keyframe_request_code = 随机 4 字母字符串
keyframe_request_remaining = idr_max_messages
```

之后地面端会把这个请求码追加到周期性 UDP 统计消息末尾，连续发送最多 `idr_max_messages` 次：

```text
timestamp:rssi_score:snr_score:fec_rec:lost:rssi:snr:num_antennas:penalty:fec_change:<idr_code>
```

当视频接收从无到有时，`alink_gs` 也会生成一次 keyframe 请求码，帮助刚开始接收的一端尽快拿到可解码的 IDR。

### 飞机端接收

飞机端 `alink_drone` 在 `process_message()` 中解析 UDP 消息。第 11 个字段如果存在，会作为 `idr_code`：

```text
case 10 -> idr_code
```

随后转换为内部 special 命令：

```text
special:request_keyframe:<idr_code>
```

再交给 `special_command_message()` 处理。

### 去重和限流

`alink_drone` 只有在以下条件满足时才会真正请求 IDR：

```text
allow_request_keyframe=1
prevSetGop > 0.5
消息类型为 request_keyframe
携带非空 idr_code
距离上次请求超过 request_keyframe_interval_ms
该 idr_code 最近没有处理过
```

相关默认配置：

```ini
allow_request_keyframe=1
request_keyframe_interval_ms=1112
idrCommandTemplate="curl localhost/request/idr"
```

去重逻辑维护最多 `MAX_CODES=5` 个近期请求码。相同 `idr_code` 在 `EXPIRY_TIME_MS=1000` ms 内不会重复触发，避免地面端连续发送同一个请求码时飞机端重复打 IDR。

真正执行的是：

```sh
curl localhost/request/idr
```

### 本机 TX dropped 触发

飞机端还会周期读取本机发送队列丢包统计：

```text
/sys/class/net/wlan0/statistics/tx_dropped
```

如果发现新的 `tx_dropped`，并且满足以下条件，也会请求 IDR：

```text
allow_rq_kf_by_tx_d=1
prevSetGop > 0.5
距离上次请求超过 request_keyframe_interval_ms
```

相关配置：

```ini
allow_rq_kf_by_tx_d=1
check_xtx_period_ms=2250
request_keyframe_interval_ms=1112
```

这个触发源和地面端 lost-packet 触发共享同一个 `last_keyframe_request_time` 限流时间。

### Profile 切换触发

profile 切换时也可以请求 IDR，但默认关闭：

```ini
idr_every_change=0
```

如果改成 `1`，`apply_profile()` 在升档或降档应用新 profile 后会执行 `idrCommandTemplate`。这有助于参数切换后快速刷新参考链，但会增加 I 帧尖峰和链路压力。

### 注意事项

- IDR 请求会产生新的大帧，不会重传已经丢失的视频包。
- 低 MCS 或链路拥塞时，频繁 IDR 可能让发送队列堆积，反而增加延迟。
- `prevSetGop > 0.5` 是源码里的保护条件；非常短 GOP 或全 I 模式下，keyframe 请求可能被跳过。
- `request_keyframe_interval_ms` 应该和 GOP、码率、MCS/FEC 档位一起调，不能只追求更短间隔。

## 验证命令

确认 Majestic 是否提供运行时 API：

```sh
strings /usr/bin/majestic | grep -E 'api/v1/set|request/idr|reload_sdk'
```

手动测试运行时改码率：

```sh
curl -s 'http://localhost/api/v1/set?video0.bitrate=8000'
cli -g .video0.bitrate
```

请求关键帧：

```sh
curl localhost/request/idr
```

查看 WFB 当前 FEC 和 radio 参数：

```sh
wfb_tx_cmd 8000 get_fec
wfb_tx_cmd 8000 get_radio
```

查看本机 TX dropped：

```sh
cat /sys/class/net/wlan0/statistics/tx_dropped
```

## 注意事项

`adaptive-link` 的效果高度依赖 profile 配置。如果 profile 中的码率超过实际链路能力，自动切档也无法避免卡顿或丢包。

需要特别谨慎配置 TX power。`txprofiles.conf` 和 `wlan_adapters.yaml` 中的功率值与具体网卡、驱动和散热条件有关，过高可能导致网卡过热或损坏。

如果只使用 `cli -s` 修改 `video0.bitrate`，不能等同于 `adaptive-link` 的动态调节路径。动态调节应优先使用 Majestic HTTP API：

```sh
curl -s 'http://localhost/api/v1/set?video0.bitrate=<kbit/s>'
```
