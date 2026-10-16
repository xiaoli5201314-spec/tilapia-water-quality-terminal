/* 文件：status_led.c */
/* 功能：实现 RUN 心跳灯和 ERR 故障灯的低有效电平控制。 */
/* RUN 由主循环心跳控制，ERR 由明确的故障条件控制。 */
#include "status_led.h"
#include "board_config.h"

/* 将逻辑亮灭转换成低有效 LED 电平。 */
static void StatusLed_Write(GPIO_TypeDef *port, uint16_t pin, bool on)
{
  HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* 初始化时将 RUN、ERR 都置高熄灭。 */
void StatusLed_Init(void)
{
  StatusLed_SetRun(false);
  StatusLed_SetError(false);
}

/* on=true 时 RUN 低电平点亮；主循环心跳会周期更新此状态。 */
void StatusLed_SetRun(bool on)
{
  StatusLed_Write(BOARD_RUN_LED_PORT, BOARD_RUN_LED_PIN, on);
}

/* on=true 时 ERR 低电平点亮，false 熄灭；该接口不自行判定故障。 */
void StatusLed_SetError(bool on)
{
  StatusLed_Write(BOARD_ERR_LED_PORT, BOARD_ERR_LED_PIN, on);
}
