# 构建与运行

## Keil 固件

环境：Keil MDK 5、Arm Compiler 5、STM32F4 对应设备支持包。

1. 配置 `firmware/Application/Inc/network_config.h` 中的 Wi-Fi 与 MQTT 参数。
2. 核对 `board_config.h` 和 `.ioc` 中的板级映射。
3. 打开 `firmware/MDK-ARM/XIAOLI-CODE.uvprojx`，选择 `XIAOLI-CODE` 目标并重新构建。

也可从仓库根目录运行脚本：

```powershell
./firmware/tools/Build.ps1 -KeilPath "C:\Keil_v5\UV4\UV4.exe"
```

脚本同步应用源码到工程，检查时钟、串口、Flash 保留区及源文件成员，执行完整重新编译，再核验 HEX 文件地址范围。也可通过环境变量 `KEIL_UV4` 指定工具位置。

## Python 上位机

建议 Python 3.12 或更高版本：

```powershell
python -m venv .venv
./.venv/Scripts/Activate.ps1
python -m pip install -r desktop/requirements.txt
$env:WATER_MQTT_HOST = "127.0.0.1"
$env:WATER_MQTT_PORT = "1883"
$env:WATER_MQTT_USER = "YOUR_MQTT_USER"
$env:WATER_MQTT_PASSWORD = "YOUR_MQTT_PASSWORD"
$env:WATER_DEVICE_ID = "YOUR_DEVICE_UID"
python desktop/app.py
```

`desktop/.env.example` 汇总 MQTT 参数；应用读取进程环境变量。设备 ID 应与固件通过 UID 生成的主题一致。

| 环境变量 | 作用 |
| --- | --- |
| `WATER_MQTT_HOST` / `WATER_MQTT_PORT` | MQTT 服务地址 / 端口 |
| `WATER_MQTT_USER` / `WATER_MQTT_PASSWORD` | MQTT 认证 |
| `WATER_DEVICE_ID` | 订阅与控制的设备 ID |
| `WATER_CONTROL_PORT` | 本机服务的起始端口，默认 5070 |
| `WATER_NO_BROWSER=1` | 禁止启动时自动打开浏览器 |

应用监听本机回环地址，SQLite 数据在运行时创建。浏览器包含水质概览、历史数据、采集设置、清洗设置、机械控制、命令记录与 CSV 导出。

## 自动化检查

从根目录先执行固件主机测试，生成跨语言契约测试使用的状态报文：

```sh
sh firmware/tests/run_tests.sh
```

然后运行上位机测试：

```sh
python -m pip install -r requirements-dev.txt
cd desktop
python -m unittest discover -s tests -p test_control.py
python -m unittest discover -s tests -p test_regressions.py
python -m unittest discover -s tests -p test_firmware_contract.py
python -m playwright install chromium
python -m unittest discover -s tests -p test_browser.py
```

主机测试使用 HAL 桩和本地 HTTP 测试服务，可独立检查协议和交互逻辑。硬件模块入口位于 `module-tests/`，按对应外设连接，在固件工程中选用单一测试入口进行板级调试。

[返回项目首页](../README.md)
