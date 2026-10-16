# 发布验证记录

验证日期：2026-10-16。以下结果来自本次发布目录中的代码。

## Keil 编译

命令：

```powershell
./firmware/tools/Build.ps1 -KeilPath "<Keil installation>/UV4/UV4.exe"
```

环境：Keil MDK 5，Arm Compiler `V5.06 update 5 (build 528)`。

结果：

```text
0 Error(s), 0 Warning(s)
Code=28068 RO-data=2052 RW-data=264 ZI-data=18864
PASS: IOC, generated clocks/UARTs and Keil source membership.
PASS: Intel HEX 30384 bytes; end 0x080076B0 <= 0x080E0000.
```

## 固件主机逻辑

环境：WSL Ubuntu 22.04、GCC 11.4.0；启用 `-Wall -Wextra -Werror -pedantic` 和 AddressSanitizer / UndefinedBehaviorSanitizer。

执行 `sh firmware/tests/run_tests.sh` 通过四组测试程序，其中网络恢复程序运行四种场景。覆盖内容包括：

- GPIO 安全状态、无效通道、动作超时、换向保护、计时回绕与停止。
- Modbus CRC、寄存器数量、异常帧和真实寄存器格式解码。
- AT 终止行、超时、立即应答、接收溢出和串口分帧。
- JSON 语法、重复字段、类型/范围、命令有效期与重复命令。
- 采集窗口、清洗流程、Flash 校验与存储失败回滚。
- 状态分片冻结、容量预算、延迟发布确认和网络会话恢复。

## Python 与浏览器

Python 3.14 环境执行：

| 测试入口 | 结果 |
| --- | --- |
| `test_control.py` | 21 项通过 |
| `test_regressions.py` | 8 项通过 |
| `test_firmware_contract.py` | 1 项通过 |
| `test_browser.py` | 8 项通过 |

固件契约测试读取 C 测试实际导出的四片报文，检查 MQTT 容量与 Python 状态组装。浏览器测试使用 Playwright Chromium 和独立的本机测试服务，覆盖草稿保留、时间选择、命令确认、方向映射、断连处理与桌面/移动端页面布局。

验证结果对应软件构建与主机测试，可通过[构建文档](build-and-run.md)中的命令复现。硬件接线、网络服务和机械参数由具体部署配置决定。

[返回项目首页](../README.md)
