/* 文件：alarm_light.c */
/* 功能：实现三色报警灯互斥点亮和全部熄灭。 */
/* 三色灯互斥输出，告警阈值留给后续业务层决定。 */
#include "alarm_light.h"
#include "board_config.h"

static AlarmLightColor current_color = ALARM_LIGHT_OFF;

/* 高电平关闭三个输出，为换色提供全灭中间状态。 */
static void AlarmLight_AllOff(void)
{
  HAL_GPIO_WritePin(BOARD_ALARM_RED_PORT, BOARD_ALARM_RED_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(BOARD_ALARM_YELLOW_PORT, BOARD_ALARM_YELLOW_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(BOARD_ALARM_GREEN_PORT, BOARD_ALARM_GREEN_PIN, GPIO_PIN_SET);
}

/* 初始化为三色全灭，不默认宣告水质或网络正常。 */
void AlarmLight_Init(void)
{
  AlarmLight_Set(ALARM_LIGHT_OFF);
}

/* 选择 color 对应的一种灯色，其他灯熄灭；OFF 或无效值使三色全灭，低电平点亮。 */
void AlarmLight_Set(AlarmLightColor color)
{
  /* 先全灭再点亮目标色，避免切换瞬间多色同时亮。 */
  AlarmLight_AllOff();
  if ((unsigned int)color > (unsigned int)ALARM_LIGHT_GREEN)
  {
    color = ALARM_LIGHT_OFF;
  }

  if (color == ALARM_LIGHT_RED)
  {
    HAL_GPIO_WritePin(BOARD_ALARM_RED_PORT, BOARD_ALARM_RED_PIN, GPIO_PIN_RESET);
  }
  else if (color == ALARM_LIGHT_YELLOW)
  {
    HAL_GPIO_WritePin(BOARD_ALARM_YELLOW_PORT, BOARD_ALARM_YELLOW_PIN, GPIO_PIN_RESET);
  }
  else if (color == ALARM_LIGHT_GREEN)
  {
    HAL_GPIO_WritePin(BOARD_ALARM_GREEN_PORT, BOARD_ALARM_GREEN_PIN, GPIO_PIN_RESET);
  }

  current_color = color;
}

/* 返回上次设置的灯色，不检测外接灯具是否实际点亮。 */
AlarmLightColor AlarmLight_Get(void)
{
  return current_color;
}
