/* 文件：sensor_power.h */
/* 功能：两路水质传感器电源的独立开关接口。 */
/* 两路传感器供电：通道 1=PD11，通道 2=PD10，低电平供电。 */
#ifndef __SENSOR_POWER_H__
#define __SENSOR_POWER_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  SENSOR_CHANNEL_1 = 0,
  SENSOR_CHANNEL_2
} SensorPowerChannel;

/** 将两路控制脚置高，关闭传感器供电并清除软件供电标记。 */
void SensorPower_Init(void);
/** 设置 channel 供电状态：on=true 输出低电平供电，false 断电；无效通道忽略。 */
void SensorPower_Set(SensorPowerChannel channel, bool on);
/** 开启两路供电，不等待传感器预热；业务层必须安排稳定时间后再查询。 */
void SensorPower_AllOn(void);
/** 关闭两路传感器供电，不改变 RS485 接线和其他执行器状态。 */
void SensorPower_AllOff(void);
/** 返回软件记录的供电命令状态，无效通道返回 false；不是实际电压检测结果。 */
bool SensorPower_IsOn(SensorPowerChannel channel);

#endif /* __SENSOR_POWER_H__ */
