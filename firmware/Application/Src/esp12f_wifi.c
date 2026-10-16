/* 文件：esp12f_wifi.c */
/* 功能：实现 ESP-12F Wi-Fi 模块的 AT 命令基础封装。 */
/* ESP-12F AT 基础接口；命令成功仅表示模块应答，不代表 MQTT 已连接。 */
#include "esp12f_wifi.h"
#include "board_config.h"
#include "at_command.h"
static bool at_responsive;  /* 最近一次非 BUSY AT 命令结果，不等于已连接热点。 */

/* 清除 AT 应答记录，不复位 ESP-12F，也不主动连接 Wi-Fi。 */
void Esp12fWifi_Init(void) { at_responsive = false; }
/* 复用通用 AT 事务，保留超时和 BUSY 的区别。 */
HAL_StatusTypeDef Esp12fWifi_SendCommand(const char *command, char *response,
                                       uint16_t response_capacity, uint32_t timeout)
{
  HAL_StatusTypeDef status = AtCommand_Execute(BOARD_WIFI_UART, command, response,
                                              response_capacity, timeout);
  if (status == HAL_OK) { at_responsive = true; }
  else if (status != HAL_BUSY) { at_responsive = false; }
  return status;
}
/* 返回最近一次非 BUSY 命令是否成功，无自动过期检测；不是 Wi-Fi 或 MQTT 在线状态。 */
bool Esp12fWifi_IsReady(void) { return at_responsive; }
