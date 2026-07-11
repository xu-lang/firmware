# RTL8812AU TX/RX Path Test

This note records the manual test flow for RTL8812AU AP mode with selectable RF paths.

## Paths

The patched driver exposes:

```sh
/proc/net/rtl88xxau/wlan0/tx_path
```

Supported values:

```sh
echo a  > /proc/net/rtl88xxau/wlan0/tx_path  # path A only
echo b  > /proc/net/rtl88xxau/wlan0/tx_path  # path B only
echo ab > /proc/net/rtl88xxau/wlan0/tx_path  # paths A+B
cat /proc/net/rtl88xxau/wlan0/tx_path
```

The proc node sets both TX and RX path registers:

| Register | Address | A | B | AB |
| --- | --- | --- | --- | --- |
| `rTxPath_Jaguar` | `0x80c` | low16 `0x1111` | low16 `0x2222` | low16 `0x3333` |
| `rRxPath_Jaguar` | `0x808` | low8 `0x11` | low8 `0x22` | low8 `0x33` |
| `rCCK_RX_Jaguar` | `0xa04` | `0` | `1` | `0` |

## Start AP

Start AP with B path only:

```sh
/mnt/mmcblk0p1/start-ap.sh OpenIPC_AP 12345678 b
```

Start AP with both paths:

```sh
/mnt/mmcblk0p1/start-ap.sh OpenIPC_AP 12345678 ab
```

If needed, set the path manually after AP is up:

```sh
echo b > /proc/net/rtl88xxau/wlan0/tx_path
cat /proc/net/rtl88xxau/wlan0/tx_path
```

Expected output for B path:

```text
b
```

## Verify AP

```sh
iw dev wlan0 info
```

Expected:

```text
type AP
channel 149 (5745 MHz), width: 40 MHz
```

If AP falls back to another channel, restart it with the 149/HT40 hostapd config and re-apply the path:

```sh
killall hostapd 2>/dev/null
/mnt/mmcblk0p1/hostapd /mnt/mmcblk0p1/rtl8812au-stbc-test/hostapd-stbc-149.conf >/dev/null 2>&1 &
sleep 5
echo b > /proc/net/rtl88xxau/wlan0/tx_path
iw dev wlan0 info
cat /proc/net/rtl88xxau/wlan0/tx_path
```

## Verify Registers

```sh
echo "80c 4" > /proc/net/rtl88xxau/wlan0/read_reg
cat /proc/net/rtl88xxau/wlan0/read_reg

echo "808 4" > /proc/net/rtl88xxau/wlan0/read_reg
cat /proc/net/rtl88xxau/wlan0/read_reg

echo "a04 4" > /proc/net/rtl88xxau/wlan0/read_reg
cat /proc/net/rtl88xxau/wlan0/read_reg
```

For B path, expected values are:

```text
0x80c low16 = 0x2222
0x808 low8  = 0x22
0xa04       = 0x1
```

Example observed B-path values:

```text
rtw_read32(0x808)=0x3e028222
rtw_read32(0xa04)=0x1
```

## PC Setup

Connect the PC to:

```text
SSID: OpenIPC_AP
Password: 12345678
```

Expected PC IPv4:

```text
192.168.4.28
```

If the PC has no IPv4, start DHCP on the board:

```sh
ip addr add 192.168.4.1/24 dev wlan0 2>/dev/null
killall dnsmasq 2>/dev/null
/mnt/mmcblk0p1/dnsmasq --interface=wlan0 \
  --dhcp-range=192.168.4.2,192.168.4.100,255.255.255.0,24h \
  --dhcp-option=3,192.168.4.1 \
  --dhcp-option=6,192.168.4.1
```

Confirm connectivity:

```sh
ping -c 2 -W 2 192.168.4.28
```

## iperf3 Test

On Windows PC:

```powershell
iperf3.exe -s -p 5600
```

On the board:

```sh
cd /mnt/mmcblk0p1/iperf3-tmp
LD_LIBRARY_PATH=./lib ./iperf3.bin \
  -c 192.168.4.28 \
  -u -b 100M -l 1215 \
  -p 5600 -t 12 -i 1 \
  --bind 192.168.4.1
```

## Observed Results

B path only, UDP 100M target:

```text
sender:   78.3 Mbits/sec
receiver: 78.0 Mbits/sec
loss:     28/96626 = 0.029%
jitter:   0.095 ms
```

B path only, lower-rate checks:

```text
10M: receiver 9.98 Mbits/sec, loss 0%
20M: receiver 20.0 Mbits/sec, loss 0.085%
50M: receiver 50.0 Mbits/sec, loss 0%
```

## Stop AP

```sh
/mnt/mmcblk0p1/stop-ap.sh
```
