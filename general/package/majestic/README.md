# Majestic 配置说明

本文说明 OpenIPC 固件中 `majestic.yaml` 的主要配置项，以及 `CBR` 等码率控制模式的工作方式。

默认配置安装到：

```text
/etc/majestic.yaml
```

运行时通常用 `cli` 查询或修改配置，例如：

```sh
cli -g .video0.rcMode
cli -s .video0.rcMode cbr
```

注意：部分 FPV 脚本会在首次启动、启用 FPV 模式或启动 wifibroadcast 时重写视频配置，例如把 `rcMode` 从默认的 `vbr` 改成 `cbr`。

## system

```yaml
system:
  webPort: 80
  httpsPort: 443
  logLevel: debug
```

- `webPort`：Web UI 的 HTTP 端口。
- `httpsPort`：Web UI 的 HTTPS 端口。只有证书和私钥存在时才会启动 TLS 服务。
- `logLevel`：日志级别。常见值包括 `debug`、`info`、`warn`、`error`。

## isp

```yaml
isp:
  antiFlicker: disabled
```

- `antiFlicker`：ISP 抗频闪设置。常见值可能包括 `disabled`、`50hz`、`60hz`，具体取决于平台支持。
- `sensorConfig`：可选的传感器/ISP 配置文件。FPV 脚本可能设置为 `/etc/sensors/imx415_fpv.bin` 这类文件。
- `exposure`：可选曝光参数，部分 FPV 配置会设置该值。

## image

```yaml
image:
  mirror: false
  flip: false
  rotate: 0
  contrast: 50
  hue: 50
  saturation: 50
  luminance: 50
```

- `mirror`：水平镜像。
- `flip`：垂直翻转。
- `rotate`：旋转图像，是否生效取决于平台支持。
- `contrast`：对比度。
- `hue`：色调。
- `saturation`：饱和度。
- `luminance`：亮度/亮度增益。

## video0

`video0` 是主码流。

```yaml
video0:
  enabled: true
  codec: h264
  fps: 20
  bitrate: 4096
  rcMode: vbr
  gopSize: 1
```

- `enabled`：是否启用主码流。
- `codec`：视频编码格式，常见值为 `h264` 或 `h265`。
- `size`：可选输出分辨率，例如 `1920x1080`。如果省略，可能使用平台默认值或传感器相关默认值。
- `fps`：目标帧率。
- `bitrate`：目标码率，单位是 kbit/s。例如 `4096` 约等于 4 Mbit/s。
- `rcMode`：码率控制模式，见本文后面的“码率控制”。
- `gopSize`：Majestic 使用的 GOP 时长系数。通常会换算成编码器的 GOP 帧数：`fps * gopSize`。在已测试的 SigmaStar 构建上，`gopSize: 0` 会被 Majestic 映射为 `GOP1`。

示例：

```yaml
video0:
  codec: h265
  size: 1920x1080
  fps: 90
  bitrate: 4096
  rcMode: cbr
  gopSize: 1
```

该配置会映射为 90 帧 GOP：

```text
GOP = fps * gopSize = 90 * 1 = 90
```

如果希望 90fps 下每一帧都是 I 帧，实测可设置：

```yaml
gopSize: 0
```

Majestic 日志会显示：

```text
GOP1
```

## video1

`video1` 是可选副码流。

```yaml
video1:
  enabled: false
  codec: h264
  size: 704x576
  fps: 15
```

- `enabled`：是否启用副码流。
- `codec`：副码流编码格式。
- `size`：副码流分辨率。
- `fps`：副码流帧率。

资源受限设备上通常会关闭副码流，以节省内存和编码器资源。

## jpeg

```yaml
jpeg:
  enabled: true
  qfactor: 50
  fps: 5
```

- `enabled`：是否启用 JPEG 抓图或相关输出，具体行为取决于平台支持。
- `qfactor`：JPEG 质量因子。数值越高，通常质量越好、文件越大。
- `fps`：JPEG 抓图帧率。

## osd

```yaml
osd:
  enabled: false
  font: /usr/share/fonts/truetype/UbuntuMono-Regular.ttf
  template: '%d.%m.%Y %H:%M:%S'
  posX: 16
  posY: 16
```

- `enabled`：是否启用 OSD 叠加。
- `font`：OSD 文本字体文件。
- `template`：OSD 文本或时间模板。
- `posX`：OSD 横向位置。
- `posY`：OSD 纵向位置。

## audio

```yaml
audio:
  enabled: false
  volume: 30
  srate: 8000
  codec: opus
  outputEnabled: false
  outputVolume: 30
```

- `enabled`：是否启用音频采集和编码。
- `volume`：输入音量。
- `srate`：采样率。
- `codec`：音频编码格式，例如 `opus`。
- `outputEnabled`：是否启用音频输出路径，取决于平台支持。
- `outputVolume`：输出音量。

## rtsp

```yaml
rtsp:
  enabled: true
  port: 554
```

- `enabled`：是否启用 RTSP 服务。
- `port`：RTSP 监听端口。

## nightMode

```yaml
nightMode:
  colorToGray: true
  irCutSingleInvert: false
  lightMonitor: false
  lightSensorInvert: false
```

- `colorToGray`：夜视模式下是否转灰度。
- `irCutSingleInvert`：是否反转单线 IR-cut GPIO 逻辑。
- `lightMonitor`：是否启用光照监测。
- `lightSensorInvert`：是否反转光敏传感器逻辑。

## motionDetect

```yaml
motionDetect:
  enabled: false
  visualize: false
  debug: false
```

- `enabled`：是否启用移动侦测。
- `visualize`：是否显示或输出移动侦测可视化信息。
- `debug`：是否启用移动侦测调试信息。

## records

```yaml
records:
  enabled: false
  path: /mnt/mmcblk0p1/%F
  split: 20
  maxUsage: 95
```

- `enabled`：是否启用本地录像。
- `path`：录像保存路径。Majestic 可能支持日期/时间占位符。
- `split`：录像分段时长或分段间隔，具体行为取决于构建版本。
- `maxUsage`：存储使用率上限百分比，用于触发清理或停止录像策略。

## outgoing

```yaml
outgoing:
  enabled: false
```

FPV 配置常见扩展：

```yaml
outgoing:
  enabled: true
  server: udp://192.168.1.3:5600
```

- `enabled`：是否启用外发码流。
- `server`：外发目标地址，FPV 常见形式是 `udp://host:port`。
- `wfb`：wifibroadcast 集成使用的开关，用于将视频送入 WFB 链路。

## watchdog

```yaml
watchdog:
  enabled: true
  timeout: 300
```

- `enabled`：是否启用 watchdog 集成。
- `timeout`：watchdog 超时时间，单位秒。

## hls

```yaml
hls:
  enabled: false
```

- `enabled`：是否启用 HLS 输出，取决于构建支持。

## onvif

```yaml
onvif:
  enabled: true
```

- `enabled`：是否启用 ONVIF 服务。
- `username`：可选的 ONVIF 明文兼容用户名。
- `password`：可选的 ONVIF 明文兼容密码。

默认配置文件中对 `username`/`password` 有安全提示：这是为了兼容部分较老的 ONVIF 客户端。保持密码为空时使用默认认证行为。

## fpv

FPV 配置可能加入：

```yaml
fpv:
  enabled: true
  noiseLevel: 0
```

- `enabled`：标记 FPV 模式启用。
- `noiseLevel`：FPV 相关噪声/降噪级别，具体由支持它的组件解释。

FPV 脚本可能还会设置：

```sh
cli -s .video0.codec h265
cli -s .video0.fps 60
cli -s .video0.bitrate 8000
cli -s .video0.rcMode cbr
cli -s .outgoing.enabled true
```

## 码率控制

`rcMode` 用于选择编码器如何在码率和画质之间做取舍。

OpenIPC 菜单中常见值包括：

- `cbr`：Constant Bitrate，固定/目标码率。
- `vbr`：Variable Bitrate，可变码率。
- `avbr`：Adaptive Variable Bitrate，自适应可变码率。

仓库中 Majestic 默认配置使用：

```yaml
rcMode: vbr
```

但 FPV 相关脚本可能会改成：

```yaml
rcMode: cbr
```

### CBR

`CBR` 是 Constant Bitrate，通常应理解为“目标码率模式”，不是严格按字节锁死的绝对码率。

例如：

```yaml
bitrate: 4096
rcMode: cbr
```

Majestic 会把约 4 Mbit/s 的目标码率传给硬件编码器的码率控制配置。

编码器会尝试通过调整 `QP` 让输出码率接近目标值。

`QP` 是 Quantization Parameter，量化参数：

- QP 越低：压缩越轻，画质越好，码率越高。
- QP 越高：压缩越重，画质越差，码率越低。

当画面很简单、实际码率低于目标值时，编码器通常会降低 QP，提高画质并尝试使用更多码率。如果 QP 已经降到 `minQp`，画面仍然很简单，那么实际输出仍可能低于目标码率。

当画面很复杂、实际码率高于目标值时，编码器通常会提高 QP，加大压缩强度。如果 QP 已经升到 `maxQp`，码流仍可能超过目标值，或者画质明显下降。

在已测试的 SigmaStar 构建中，Majestic 日志会出现类似：

```text
[minQp: 12, maxQp: 48, qpDelta: -4, slice: 0]
```

这说明编码器的 QP 调整范围是有限的。

CBR 适合 FPV 或带宽受限链路，因为发送端会尽量让带宽更可预测。但它不保证任何场景下每秒码率都精确等于配置值。高复杂度画面、高帧率、硬件/驱动限制或参数不完整时，都可能出现明显波动。

### VBR

`VBR` 是 Variable Bitrate，可变码率。它通常更偏向保持画质，而不是固定带宽。

- 简单画面码率较低。
- 复杂画面码率较高。
- 在网络带宽充足时，画质通常比 CBR 更稳定。

VBR 适合普通 IP 摄像机场景，因此仓库中的 Majestic 默认配置使用 `vbr`。

### AVBR

`AVBR` 是 Adaptive Variable Bitrate，自适应可变码率。

它通常在 VBR 基础上加入更强的自适应策略，根据画面复杂度、运动量、缓冲状态等动态调节码率和质量。

具体行为依赖 SoC 厂商 SDK 和 Majestic 的平台适配实现。

## FPV 模式为什么会变成 CBR

仓库默认 Majestic 配置是 `vbr`，但 FPV 脚本会切换到 `cbr`。

例如：

```sh
general/package/wifibroadcast-ng/files/wifibroadcast:
cli -s .video0.rcMode cbr
```

```sh
general/package/rubyfpv/files/tweaksys:
cli -s .video0.rcMode cbr
```

```sh
general/package/legacy/datalink/files/tweaksys:
cli -s .video0.rcMode cbr
```

所以一个板子即使最初安装时是默认 `vbr`，只要运行过 FPV 配置、`tweaksys` 或 wifibroadcast 初始化逻辑，后续 `/etc/majestic.yaml` 中就可能变成 `cbr`。
