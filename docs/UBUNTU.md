# 在 Ubuntu 上构建与刷写

结论：**可以，而且已经在 Ubuntu 上验证过** —— 本仓库的 GitHub Actions 每次都在
`ubuntu-latest` 上把 `esp32 / esp32s3 / esp32c6 / esp32c5` 四个环境完整编译通过。
Windows 只是原作者的习惯，构建链本身没有 Windows 依赖。

## 0. 先确认要移植的是什么

| 组件 | 跑在哪 | 说明 |
|---|---|---|
| 固件 | **ESP32 芯片** | 和宿主机操作系统无关，编出来就能刷 |
| 构建工具链 | Ubuntu | PlatformIO + ESP-IDF，Linux 原生支持 |
| 管理页面 | 芯片自带的 Web 服务 | 用浏览器访问 `192.168.4.1`，不挑系统 |
| `tools/*.exe`、`libwifi_mod.bat` | Windows | **构建不引用**，可以忽略 |

## 1. VMware 里先把 USB 设备直通进去

VMware Fusion/Workstation 默认不把 USB 设备交给虚拟机，必须手动连接：

1. 插上 ESP32 开发板 → 菜单 **VM → Removable Devices → 你的串口设备 → Connect**
2. 插上 USB 网卡 → 同样 Connect
3. 在 Ubuntu 里确认：

```bash
lsusb                    # 能看到设备
dmesg | tail -20         # 看有没有 ttyUSB0 / wlan0
```

常见串口芯片：`CP210x`、`CH340`、`FTDI`。ESP32-S3/C3 的原生 USB 会显示为 `ttyACM0`。

## 2. 串口权限

```bash
sudo usermod -aG dialout $USER
# 重新登录（或 newgrp dialout）后生效
```

不做的后果：`pio run -t upload` 报 `Permission denied: /dev/ttyUSB0`。

## 3. 安装 PlatformIO

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip git curl

curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py

# 加进 PATH（写进 ~/.bashrc）
echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
pio --version
```

## 4. 编译

```bash
git clone https://github.com/furs0x/ESP32WifiPhisher-audited.git
cd ESP32WifiPhisher-audited

# 文件系统 + 固件（不再需要任何口令）
pio run -e esp32s3 -t buildfs
pio run -e esp32s3

ls dist/                      # esp32s3-merged.bin
```

把 `esp32s3` 换成 `esp32` / `esp32c6` / `esp32c5` / `cardputer` 即可。

## 5. ⚠️ 关键一步：修 SDK 的原始帧校验

**不做这一步，固件能编译、能启动、门户页也能用，但 deauth / beacon / evil-twin
注入全部静默失效。**

原因：ESP-IDF 预编译的 `libnet80211.a` 里有一个**强符号**
`ieee80211_raw_frame_sanity_check()`，它会拒绝原始管理帧。项目在
`src/wifi/wifiMng.c` 里给了自己的实现（返回 0），但链接器优先选强符号 →
SDK 那份生效 → 注入被拦。

原作者用 `tools/libwifi_mod.bat`（Windows）通过 `objcopy --weaken-symbol`
把 SDK 那份弱化。Linux 下用等价脚本：

```bash
./scripts/patch_net80211.sh            # 所有已安装架构
./scripts/patch_net80211.sh esp32s3    # 只修一个
./scripts/patch_net80211.sh --restore  # 还原
```

它会：
- 找到 `~/.platformio/packages/framework-espidf/components/esp_wifi/lib/<arch>/libnet80211.a`
- 备份成 `.orig`（只备份一次）
- 用对应工具链的 `objcopy --weaken-symbol=ieee80211_raw_frame_sanity_check` 重建
- 已经弱化过的跳过；`--restore` 还原

改完**重新编译一次**才生效。

> 注意：这个脚本改的是 PlatformIO 的**共享包目录**，和 Windows 的 .bat 行为一致。
> 同一台机器上其它用到 ESP-IDF 的项目也会受影响，介意就给本项目单独一套
> `PLATFORMIO_CORE_DIR`。

## 6. 刷写

```bash
# 方式 A：PlatformIO 直接烧 app（快，但不动 bootloader）
pio run -e esp32s3 -t upload

# 方式 B：整片烧合并镜像（推荐，换机型/救砖都用这个）
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 dist/esp32s3-merged.bin
```

`esptool.py` 由 PlatformIO 装在 `~/.platformio/penv/bin/`，PATH 里已经有。

刷前建议备份原厂固件：

```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 read_flash 0 0x400000 backup_orig.bin
```

## 7. 看日志

```bash
pio device monitor -b 115200
# 或
screen /dev/ttyUSB0 115200
```

## 8. USB 网卡（如果你要的是 Ubuntu 侧的抓包/注入）

这条和 ESP32 固件是**两条独立的线**：

- **ESP32 方案**：芯片自己发包，Ubuntu 只负责编译刷写，网卡用不上。
- **Ubuntu 网卡方案**：用 `aircrack-ng` 套件在本机做 monitor mode / 注入，
  和本仓库无关，但可以互补（比如用网卡抓包、ESP32 做 evil twin）。

网卡能不能用，取决于芯片：

```bash
lsusb                      # 看型号
sudo apt install -y aircrack-ng
sudo airmon-ng start wlan0
sudo airodump-ng wlan0mon
```

| 芯片 | 支持情况 |
|---|---|
| Atheros AR9271 / AR7010 | 内核自带 `ath9k_htc`，开箱即用 |
| RTL8812AU / RTL8814AU | 需装 `rtl8812au-dkms`，**VMware 里常因 USB 直通不稳掉线** |
| RTL8188EUS | 内核 `rtl8188eu` 或 `8188eu-dkms` |
| MediaTek MT7601U | 内核自带，但注入支持差 |
| Intel 内置网卡 | **不支持 monitor mode + 注入**，只能被动监听 |

VM 里跑 monitor mode 的坑：USB 直通抖动会导致 `wlan0mon` 消失，建议给 VM
至少 2 核 4GB，并在 VMware 里把 USB 兼容性设为 **USB 3.1**。

## 9. 已知差异清单

| 项目 | Windows | Ubuntu |
|---|---|---|
| 构建 | `pio run` | 相同 |
| SDK 原始帧补丁 | `tools/libwifi_mod.bat` | `scripts/patch_net80211.sh` ⬅ 新增 |
| 串口名 | `COM3` | `/dev/ttyUSB0` 或 `/dev/ttyACM0` |
| 串口权限 | 无 | `dialout` 组 |
| `tools/*.exe` | 可用 | **不能用，也不需要** |
| 换行/路径 | `\` | `/`，脚本已用 `pathlib`，无需改 |

## 10. 验证清单

```bash
pio run -e esp32s3                     # 编译通过
ls -l dist/esp32s3-merged.bin          # 4MB 左右
esptool.py --chip esp32s3 -p /dev/ttyUSB0 flash_id   # 能读到芯片
pio run -e esp32s3 -t upload           # 烧写成功
pio device monitor                     # 看到启动日志
```

启动日志里应该出现：

```
I (xxx) MAIN: ...
I (xxx) WEBSERVER: ...
```

如果串口没有输出或反复重启，检查是否按了 RST、USB 线是否支持数据。
