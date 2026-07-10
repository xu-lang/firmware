#!/bin/sh

# ============================================
# AP 模式启动脚本
# 驱动: 88XXau
# 用法: ./start-ap.sh [SSID] [密码] [tx_path]
#   tx_path: a  | b  | ab  (默认 a)
# ============================================

HOSTAPD="/mnt/mmcblk0p1/hostapd"
DNSMASQ="/mnt/mmcblk0p1/dnsmasq"
CONF_FILE="/etc/hostapd.conf"

SSID="${1:-OpenIPC_AP}"
PASS="${2:-12345678}"
TX_PATH="${3:-a}"
WLAN_IF="wlan0"

# ============================================
# 驱动功率/RFE 设置
# ============================================
DRIVER_MODULE="88XXau"
TX_POWER_OVERRIDE="50"
RFE_TYPE="1"
AMPLIFIER_TYPE_5G="0xC0"

# ============================================
# 固定 IP 配置
# ============================================
AP_NET="192.168.4"
AP_GATEWAY="192.168.4.1"

# ============================================
# 5GHz 配置（信道149，40MHz频宽）
# ============================================
HW_MODE="a"
CHANNEL="149"
BAND_NAME="5GHz (信道149, 40MHz HT40+)"
IEEE80211N="1"
IEEE80211AC="0"
HT_CAPAB="[HT40+][SHORT-GI-20][SHORT-GI-40]"
VHT_CAPAB=""
VHT_OP_CHWIDTH="0"
VHT_CENTR_FREQ=""

# ============================================
# TX Path 寄存器定义
# ============================================
BB_REG_TX_PATH="80c"  # rTxPath_Jaguar BB 寄存器地址
TX_PATH_LOW_A="1113"  # A 路低16位
TX_PATH_LOW_B="2222"  # B 路低16位
TX_PATH_LOW_AB="3333" # AB 双路低16位

case "$TX_PATH" in
    a)  TX_PATH_LOW="$TX_PATH_LOW_A"; PATH_DESC="A 路 (主路)" ;;
    b)  TX_PATH_LOW="$TX_PATH_LOW_B"; PATH_DESC="B 路 (第二路)" ;;
    ab) TX_PATH_LOW="$TX_PATH_LOW_AB"; PATH_DESC="AB 双路" ;;
    *)  echo "错误: tx_path 必须是 a/b/ab"; exit 1 ;;
esac

echo "=========================================="
echo "启动 AP 模式"
echo "SSID: $SSID"
echo "密码: $PASS"
echo "频段: $BAND_NAME"
echo "TX 路径: $PATH_DESC"
echo "驱动功率索引: $TX_POWER_OVERRIDE (强制)"
echo "RFE Type: $RFE_TYPE (强制)"
echo "5G Amplifier Type: $AMPLIFIER_TYPE_5G (强制外置 PA+LNA)"
echo "网关: $AP_GATEWAY"
echo "=========================================="

# ============================================
# 检查 hostapd
# ============================================
if [ ! -f "$HOSTAPD" ]; then
    echo "错误: 找不到 hostapd: $HOSTAPD"
    exit 1
fi

# ============================================
# 停止冲突服务
# ============================================
echo "1. 停止冲突服务..."
pkill -f "wpa_supplicant.*$WLAN_IF" 2>/dev/null
pkill -f "udhcpc.*$WLAN_IF" 2>/dev/null
killall hostapd 2>/dev/null
killall dnsmasq 2>/dev/null
ip link set $WLAN_IF down 2>/dev/null

# ============================================
# 卸载并重新加载驱动，设置功率/RFE
# ============================================
echo "2. 卸载驱动 $DRIVER_MODULE ..."
modprobe -r $DRIVER_MODULE 2>/dev/null
sleep 1

echo "3. 加载驱动并设置 tx_pwr_idx_override=$TX_POWER_OVERRIDE, RFE=$RFE_TYPE, amp5g=$AMPLIFIER_TYPE_5G ..."
modprobe $DRIVER_MODULE \
    rtw_tx_pwr_idx_override=$TX_POWER_OVERRIDE \
    rtw_RFE_type=$RFE_TYPE \
    rtw_amplifier_type_5g=$AMPLIFIER_TYPE_5G \
    rtw_drv_log_level=5

if [ $? -ne 0 ]; then
    echo "警告: modprobe 加载失败，尝试使用已加载的驱动"
fi

sleep 2

CURRENT_TX_POWER=$(cat /sys/module/$DRIVER_MODULE/parameters/rtw_tx_pwr_idx_override 2>/dev/null)
CURRENT_RFE_TYPE=$(cat /sys/module/$DRIVER_MODULE/parameters/rtw_RFE_type 2>/dev/null)
CURRENT_AMP_5G=$(cat /sys/module/$DRIVER_MODULE/parameters/rtw_amplifier_type_5g 2>/dev/null)
echo "当前驱动功率索引: ${CURRENT_TX_POWER:-unknown}"
echo "当前 RFE Type: ${CURRENT_RFE_TYPE:-unknown}"
echo "当前 5G Amplifier Type: ${CURRENT_AMP_5G:-unknown}"

# ============================================
# 配置网卡为 AP 模式
# ============================================
echo "4. 配置网卡..."
ip link set $WLAN_IF down
iw dev $WLAN_IF set type managed
ip link set $WLAN_IF up
ip addr flush dev $WLAN_IF
ip addr add ${AP_GATEWAY}/24 dev $WLAN_IF

# ============================================
# 生成 hostapd 配置文件
# ============================================
echo "5. 生成 hostapd 配置..."
cat > $CONF_FILE << EOF
interface=$WLAN_IF
driver=nl80211
ssid=$SSID
hw_mode=$HW_MODE
channel=$CHANNEL
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=$PASS
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP

# 802.11n 支持（40MHz）
ieee80211n=$IEEE80211N
ht_capab=$HT_CAPAB

# 802.11ac 关闭（40MHz）
ieee80211ac=$IEEE80211AC
vht_capab=$VHT_CAPAB
vht_oper_chwidth=$VHT_OP_CHWIDTH
vht_oper_centr_freq_seg0_idx=$VHT_CENTR_FREQ

# 国家码
country_code=CN
EOF

# ============================================
# 启动 hostapd
# ============================================
echo "6. 启动 hostapd..."
$HOSTAPD -B $CONF_FILE
if [ $? -ne 0 ]; then
    echo "错误: hostapd 启动失败"
    echo "尝试前台运行查看错误: $HOSTAPD $CONF_FILE"
    exit 1
fi

sleep 3

# ============================================
# 设置 TX Path（写 BB 寄存器）
# ============================================
PROC_WRITE_REG="/proc/net/rtl88xxau/$WLAN_IF/write_reg"
PROC_READ_REG="/proc/net/rtl88xxau/$WLAN_IF/read_reg"

if [ -w "$PROC_WRITE_REG" ]; then
    echo "7. 设置 TX Path 为 $PATH_DESC ..."
    # 先读当前值，保留高16位
    echo "$BB_REG_TX_PATH 4" > "$PROC_READ_REG"
    sleep 1
    CURRENT_VAL=$(cat "$PROC_READ_REG" 2>/dev/null | grep -o "0x[0-9a-fA-F]\+" | tail -1)
    
    if [ -n "$CURRENT_VAL" ]; then
        # 取高16位 + 新的低16位
        HIGH_WORD=$(printf "%04x" $(( CURRENT_VAL >> 16 )) )
        NEW_VAL="0x${HIGH_WORD}${TX_PATH_LOW}"
        echo "$BB_REG_TX_PATH $(( NEW_VAL )) 4" > "$PROC_WRITE_REG"
        
        # 验证
        echo "$BB_REG_TX_PATH 4" > "$PROC_READ_REG"
        sleep 1
        RESULT=$(cat "$PROC_READ_REG" 2>/dev/null | grep -o "0x[0-9a-fA-F]\+" | tail -1)
        echo "  寄存器 0x$BB_REG_TX_PATH = $RESULT"
    else
        echo "  警告: 无法读取当前寄存器值，跳过 TX Path 设置"
    fi
else
    echo "7. 警告: write_reg proc 不可用，跳过 TX Path 设置"
fi

# ============================================
# 禁用电源管理
# ============================================
echo "8. 禁用电源管理..."
iw dev $WLAN_IF set power_save off 2>/dev/null

# ============================================
# 启动 dnsmasq
# ============================================
if [ -f "$DNSMASQ" ]; then
    echo "9. 启动 dnsmasq DHCP 服务..."
    $DNSMASQ --interface=$WLAN_IF \
             --dhcp-range=${AP_NET}.2,${AP_NET}.100,255.255.255.0,24h \
             --dhcp-option=3,${AP_GATEWAY} \
             --dhcp-option=6,${AP_GATEWAY}
else
    echo "9. 警告: 未找到 dnsmasq"
    echo "   客户端需手动设置静态 IP: ${AP_NET}.x/24, 网关: $AP_GATEWAY"
fi

# ============================================
# 显示结果
# ============================================
echo "=========================================="
echo "AP 已启动！"
echo "=========================================="
echo "SSID: $SSID"
echo "密码: $PASS"
echo "频段: $BAND_NAME"
echo "信道: $CHANNEL (5745 MHz)"
echo "频宽: 40MHz"
echo "TX 路径: $PATH_DESC"
echo "驱动: $DRIVER_MODULE"
echo "功率索引: $TX_POWER_OVERRIDE (驱动参数)"
echo "RFE Type: $RFE_TYPE"
echo "5G Amplifier Type: $AMPLIFIER_TYPE_5G"
echo "AP IP: $AP_GATEWAY"
echo "DHCP 范围: ${AP_NET}.2 - ${AP_NET}.100"
echo ""
echo "查看状态: iw dev $WLAN_IF info"
echo "停止 AP: ./stop_ap.sh"
echo "=========================================="

exit 0
