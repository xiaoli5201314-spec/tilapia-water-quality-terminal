/* 文件：air780_4g.h */
/* 功能：M100M-B2 4G 模块 AT 命令和 RDY 引脚接口。 */
/* M100M-B2 的 AT 应答与原始 RDY 输入，不代表 MQTT 在线。 */
#ifndef __AIR780_4G_H__
#define __AIR780_4G_H__

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/** 清除 AT 应答记录，不驱动模块的 RST、DTR，也不尝试注册网络。 */
void Air780_4G_Init(void);
/**
 * @brief 经 USART2 同步执行一条 AT 命令，并记录本次应答结果。
 * @param command 不含 CR/LF 的命令字符串，末尾换行由内部添加。
 * @param response 调用者提供的文本接收缓冲区，不得为 NULL。
 * @param response_capacity 缓冲区字节数，至少为 2，包含末尾空字符。
 * @param timeout 发送与等待应答的总超时，单位 ms，不接受 0 或 HAL_MAX_DELAY。
 * @return HAL_OK 为收到完整 OK 行；HAL_TIMEOUT 为超时；HAL_BUSY 为机械动作占用或底层忙；
 *         HAL_ERROR 为参数、接收或模块错误。HAL_BUSY 时保留上次 AT 应答记录。
 */
HAL_StatusTypeDef Air780_4G_SendCommand(const char *command,
                                        char *response,
                                        uint16_t response_capacity,
                                        uint32_t timeout);
/** 返回最近一次非 BUSY 命令是否成功，无自动过期检测；不是网络或 MQTT 在线状态。 */
bool Air780_4G_IsReady(void);
/** 读取 PD4 原始电平，高为 true；不推断模块已经注册网络或连接服务器。 */
bool Air780_4G_ReadRdy(void);

#endif /* __AIR780_4G_H__ */
