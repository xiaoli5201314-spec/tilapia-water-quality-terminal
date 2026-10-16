/* 文件：esp12f_wifi.h */
/* 功能：ESP-12F Wi-Fi 模块 AT 命令封装接口。 */
/* ESP-12F 同步 AT 调试接口；IsReady 仅表示最近有 AT 应答。 */
#ifndef __ESP12F_WIFI_H__
#define __ESP12F_WIFI_H__

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/** 清除 AT 应答记录，不复位 ESP-12F，也不主动连接 Wi-Fi。 */
void Esp12fWifi_Init(void);
/**
 * @brief 经 USART1 同步执行一条 AT 命令，并记录本次应答结果。
 * @param command 不含 CR/LF 的命令字符串，末尾换行由内部添加。
 * @param response 非空文本接收缓冲区；失败时可能保留部分应答。
 * @param response_capacity 缓冲区字节数，至少为 2，包含末尾空字符。
 * @param timeout 发送与等待应答的总超时，单位 ms，不接受 0 或 HAL_MAX_DELAY。
 * @return HAL_OK 为收到完整 OK 行；HAL_TIMEOUT 为超时；HAL_BUSY 为机械动作占用或底层忙；
 *         HAL_ERROR 为参数、接收或模块错误。HAL_BUSY 时保留上次 AT 应答记录。
 */
HAL_StatusTypeDef Esp12fWifi_SendCommand(const char *command,
                                         char *response,
                                         uint16_t response_capacity,
                                         uint32_t timeout);
/** 返回最近一次非 BUSY 命令是否成功，无自动过期检测；不是 Wi-Fi 或 MQTT 在线状态。 */
bool Esp12fWifi_IsReady(void);

#endif /* __ESP12F_WIFI_H__ */
