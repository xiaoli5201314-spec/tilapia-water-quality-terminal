/* 文件：relay_control.c */
/* 功能：实现两路预留继电器的低有效电平开关控制。 */
/* 预留输出仅提供开关操作，初始化为释放状态。 */
#include "relay_control.h"
#include "board_config.h"

/* 将逻辑开关转换为低有效输出，屏蔽继电器控制电平差异。 */
static void Relay_Write(GPIO_TypeDef *port, uint16_t pin, bool on)
{
  HAL_GPIO_WritePin(port, pin, on ? BOARD_RELAY_ON_LEVEL : BOARD_RELAY_OFF_LEVEL);
}

/* 初始化时释放 OUTPUT1、OUTPUT2 两路预留继电器。 */
void RelayControl_Init(void)
{
  RelayControl_AllOff();
}

/* 设置 channel 对应继电器：on=true 吸合，false 释放；无效通道忽略，不操作另一通道。 */
void RelayControl_Set(RelayChannel channel, bool on)
{
  if (channel == RELAY_CHANNEL_1)
  {
    Relay_Write(BOARD_RELAY1_PORT, BOARD_RELAY1_PIN, on);
  }
  else if (channel == RELAY_CHANNEL_2)
  {
    Relay_Write(BOARD_RELAY2_PORT, BOARD_RELAY2_PIN, on);
  }
}

/* 两路控制脚同时恢复高电平释放状态，不提供触点状态反馈。 */
void RelayControl_AllOff(void)
{
  Relay_Write(BOARD_RELAY1_PORT, BOARD_RELAY1_PIN, false);
  Relay_Write(BOARD_RELAY2_PORT, BOARD_RELAY2_PIN, false);
}
