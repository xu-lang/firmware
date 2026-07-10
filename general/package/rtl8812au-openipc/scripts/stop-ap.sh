#!/bin/sh

# ============================================
# AP 模式停止脚本
# ============================================

WLAN_IF="wlan0"

echo "正在停止 AP 模式..."

# 停止服务
killall hostapd 2>/dev/null
killall dnsmasq 2>/dev/null

# 清除 IP
ip addr flush dev $WLAN_IF 2>/dev/null

# 恢复网卡为 managed 模式
ip link set $WLAN_IF down
iw dev $WLAN_IF set type managed
ip link set $WLAN_IF up

echo "AP 已停止，网卡已恢复为 managed 模式"
iw dev $WLAN_IF info
