# 硬件映射与通信协议

## 控制器与串口

主控为 STM32F407ZGT6，外部晶振 8 MHz，系统时钟 168 MHz。外设配置以 `.ioc` 和 `Core/` 为准。

| 功能 | STM32 引脚 | 参数 |
| --- | --- | --- |
| ESP-12F Wi-Fi | USART1 TX=PA9 / RX=PA10 | 115200 / 8N1 |
| 蜂窝模块串口 | USART2 TX=PD5 / RX=PD6，RDY=PD4 | 115200 / 8N1 |
| RS485 传感器 | USART3 TX=PD8 / RX=PD9 | 9600 / 8N1 |
| 传感器供电 | PD11 / PD10 | 低电平开启 |
| 三色告警灯 | 红 PB5 / 黄 PB4 / 绿 PB3 | 低电平点亮 |
| 毛刷 | PB8 | 低电平运行 |
| 推杆驱动 | IN1=PB6 / IN2=PB7 | 10 / 01 两个方向，00 停止 |
| 继电器 | PG13 / PG12 | 低电平吸合 |

发布配置采用 `WATER_ENABLE_4G=0`，运行参数集中在 `firmware/Application/Inc/network_config.h`。

机械动作最长通电时间为 30 秒，换向保护间隔为 100 ms。参数用于驱动保护；实际动作时长应按机械结构、行程和安装方式设定。

## 传感器协议

RS485 使用 Modbus RTU，读保持寄存器功能码为 `0x03`。请求与响应都由协议层处理 CRC16，CRC 低字节先传。

| 传感器 | 从机地址 | 起始寄存器 | 读取数量 | 解码 |
| --- | --- | --- | --- | --- |
| pH | 1 | 0 | 1 | 寄存器值 / 100 |
| 溶解氧与水温 | 2 | 0 | 4 | 每项由原始整数与小数位数两个寄存器组成，值 = 原始整数 / 10^小数位数 |
| 溶氧饱和度 | 2 | 4 | 2 | 原始整数 / 10^小数位数 |

每个寄存器按大端 16 位数读取，小数位数接受 0–4。具体字节排列和范围检查见 [water_sensor.c](../firmware/Application/Src/water_sensor.c)。校验包括 CRC、地址、功能码、寄存器数量和异常应答，完整测试见 [test_baseline.c](../firmware/tests/test_baseline.c)。

## MQTT 主题

设备 ID 为三个 32 位 STM32 UID 拼接成的 24 位十六进制字符串。

| 方向 | 主题 | 内容 |
| --- | --- | --- |
| 设备 → 上位机 | `fish/device/{deviceId}/water/latest` | 四项水质测量值 |
| 上位机 → 设备 | `fish/device/{deviceId}/water/cmd` | 控制命令 |
| 设备 → 上位机 | `fish/device/{deviceId}/water/control/status` | 在线状态、采集配置、执行器状态与命令回执 |

水质报文示例：

```json
{"ph":7.65,"doMgL":6.04,"waterTemp":23.94,"doSaturation":72.44}
```

命令使用版本号、12 字符命令 ID、Unix 秒时间戳和 30 秒有效期。以下时间戳仅展示结构，发送时由上位机实时生成：

```json
{"v":1,"id":"012345abcdef","op":"brush","action":"start","duration":10,"issued":1792108800,"ttl":30}
```

支持的操作类别：

| op | 作用 |
| --- | --- |
| `sampling` | 采集模式、采集窗口、采样与发布周期 |
| `mode` | 手动 / 自动模式 |
| `brush`、`actuator` | 毛刷与推杆动作 |
| `plan` | 周期、每日或关闭计划 |
| `cleaning_config` | 清洗流程参数 |
| `cleaning` | 清洗流程控制 |
| `stop` | 停止动作 |
| `query` | 查询状态 |

命令解析入口为 [control_protocol.c](../firmware/Application/Src/control_protocol.c)；上位机参数校验入口为 [control_core.py](../desktop/control_core.py)。解析器对重复字段、未知字段、类型错误、越界数值与 JSON 语法进行检查。

## 分片与配置区

状态报文分为四片，通过同一 `bootId` 与 `seq` 组装。上位机仅在完整、有效的实时状态到达后开放相应控制，保留消息和过期分片不会更新实时控制资格。

STM32 最后一个 128 KiB 扇区作为配置区：

```text
0x08000000 ── 应用镜像 ── 0x080DFFFF
0x080E0000 ── 配置日志 ── 0x080FFFFF
```

[Verify_Project.ps1](../firmware/tools/Verify_Project.ps1) 检查工程配置，并可检查链接生成的 Intel HEX 地址边界。

[返回项目首页](../README.md)
