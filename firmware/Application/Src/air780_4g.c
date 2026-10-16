/* 文件：air780_4g.c */
/* 功能：实现 M100M-B2 4G 模块的 AT 命令收发和 RDY 电平读取。 */
/* M100M-B2 AT 基础接口，RDY 与 AT 应答状态分开管理。 */
#include "air780_4g.h"
#include "board_config.h"
#include "at_command.h"
static bool at_responsive;  /* 最近一次非 BUSY AT 命令结果，不设在线超时。 */

/* 清除 AT 应答记录，不驱动模块的 RST、DTR，也不尝试注册网络。 */
void Air780_4G_Init(void) { at_responsive = false; }
/* 复用通用 AT 事务；BUSY 未完成新事务，因此不覆盖上次应答状态。 */
HAL_StatusTypeDef Air780_4G_SendCommand(const char *command, char *response,
                                     uint16_t response_capacity, uint32_t timeout)
{
  HAL_StatusTypeDef status = AtCommand_Execute(BOARD_4G_UART, command, response,
                                              response_capacity, timeout);
  if (status == HAL_OK) { at_responsive = true; }
  else if (status != HAL_BUSY) { at_responsive = false; }
  return status;
}
/* 返回最近一次非 BUSY 命令是否成功，无自动过期检测；不是网络或 MQTT 在线状态。 */
bool Air780_4G_IsReady(void) { return at_responsive; }
/* 读取 PD4 原始电平，高为 true；不推断模块已经注册网络或连接服务器。 */
bool Air780_4G_ReadRdy(void)
{
  /* 仅返回引脚电平，不将 RDY 当作注册网络或 MQTT 成功的依据。 */
  return HAL_GPIO_ReadPin(BOARD_4G_RDY_PORT, BOARD_4G_RDY_PIN) == GPIO_PIN_SET;
}
