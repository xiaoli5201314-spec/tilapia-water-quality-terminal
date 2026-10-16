# 第三方组件

本仓库保留以下组件的原始版权与许可标识。第三方许可分别适用于对应文件。

| 组件 | 位置 | 版权 / 许可 |
| --- | --- | --- |
| STM32F4 HAL、设备支持与 ST 生成模板 | `firmware/Drivers/STM32F4xx_HAL_Driver/`、CMSIS Device 与 `Core/`、启动文件中的相应 ST 文件 | STMicroelectronics；按文件头的 BSD-3-Clause 等声明使用 |
| CMSIS Core | `firmware/Drivers/CMSIS/Include/` | Arm Limited；Apache-2.0 |
| jsmn | `firmware/ThirdParty/jsmn/jsmn.h` | Serge Zaitsev；MIT |

- [ST BSD-3-Clause 许可文本](licenses/ST-BSD-3-Clause.txt)
- [Apache-2.0 许可文本](licenses/Apache-2.0.txt)
- [jsmn 上游与固定版本](firmware/ThirdParty/jsmn/README.md)，MIT 许可全文位于 `jsmn.h` 文件头。

Python 运行依赖为 Flask 与 paho-mqtt，浏览器测试依赖 Playwright；它们通过依赖清单安装，其各自许可由对应发行包提供。
