/* 文件：sensor_power.c */
/* 功能：实现两路水质传感器电源的独立控制和软件状态记录。 */
/* 初始化断电；供电后需要在业务层等待传感器稳定再查询。 */
#include "sensor_power.h"
#include "board_config.h"

static bool sensor1_on = false;
static bool sensor2_on = false;

/* 内部调用前已校验通道，将供电命令映射为光耦的低有效输入。 */
static void SensorPower_Write(SensorPowerChannel channel, bool on)
{
  GPIO_TypeDef *port = (channel == SENSOR_CHANNEL_1) ?
                       BOARD_SENSOR1_POWER_PORT : BOARD_SENSOR2_POWER_PORT;
  uint16_t pin = (channel == SENSOR_CHANNEL_1) ?
                 BOARD_SENSOR1_POWER_PIN : BOARD_SENSOR2_POWER_PIN;
  HAL_GPIO_WritePin(port, pin, on ? BOARD_SENSOR_POWER_ON_LEVEL : BOARD_SENSOR_POWER_OFF_LEVEL);
}

/* 将两路控制脚置高，关闭传感器供电并清除软件供电标记。 */
void SensorPower_Init(void)
{
  SensorPower_AllOff();
}

/* 设置 channel 供电状态：on=true 输出低电平供电，false 断电；无效通道忽略。 */
void SensorPower_Set(SensorPowerChannel channel, bool on)
{
  /* 无效通道不允许误操作另一路传感器电源。 */
  if ((channel != SENSOR_CHANNEL_1) && (channel != SENSOR_CHANNEL_2))
  {
    return;
  }
  SensorPower_Write(channel, on);
  if (channel == SENSOR_CHANNEL_1)
  {
    sensor1_on = on;
  }
  else if (channel == SENSOR_CHANNEL_2)
  {
    sensor2_on = on;
  }
}

/* 开启两路供电，不等待传感器预热；业务层必须安排稳定时间后再查询。 */
void SensorPower_AllOn(void)
{
  SensorPower_Set(SENSOR_CHANNEL_1, true);
  SensorPower_Set(SENSOR_CHANNEL_2, true);
}

/* 关闭两路传感器供电，不改变 RS485 接线和其他执行器状态。 */
void SensorPower_AllOff(void)
{
  SensorPower_Set(SENSOR_CHANNEL_1, false);
  SensorPower_Set(SENSOR_CHANNEL_2, false);
}

/* 返回软件记录的供电命令状态，无效通道返回 false；不是实际电压检测结果。 */
bool SensorPower_IsOn(SensorPowerChannel channel)
{
  if (channel == SENSOR_CHANNEL_1) { return sensor1_on; }
  if (channel == SENSOR_CHANNEL_2) { return sensor2_on; }
  return false;
}
