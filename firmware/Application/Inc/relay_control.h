/* 文件：relay_control.h */
/* 功能：两路预留继电器输出的开关控制接口。 */
/* 预留继电器：通道 1=PG13，通道 2=PG12，低电平吸合。 */
#ifndef __RELAY_CONTROL_H__
#define __RELAY_CONTROL_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  RELAY_CHANNEL_1 = 0,
  RELAY_CHANNEL_2
} RelayChannel;

/** 初始化时释放 OUTPUT1、OUTPUT2 两路预留继电器。 */
void RelayControl_Init(void);
/** 设置 channel 对应继电器：on=true 吸合，false 释放；无效通道忽略，不操作另一通道。 */
void RelayControl_Set(RelayChannel channel, bool on);
/** 两路控制脚同时恢复高电平释放状态，不提供触点状态反馈。 */
void RelayControl_AllOff(void);

#endif /* __RELAY_CONTROL_H__ */
