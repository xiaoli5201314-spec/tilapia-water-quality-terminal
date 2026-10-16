/* 文件：brush_motor.c */
/* 功能：实现毛刷电机的低有效电平控制和时长保护。 */
/* 毛刷电机独立控制，不决定清洗顺序。 */
#include "brush_motor.h"
#include "board_config.h"
static bool motor_running;    /* 软件已下发运行命令，不是转速反馈。 */
static uint32_t started_at;   /* 本次启动的系统毫秒时刻。 */
static uint32_t duration;     /* 本次动作时限，运行期间不允许刷新。 */

/* 使 PB8 输出高电平停止毛刷，不自动执行清洗。 */
/* 输出高电平停止毛刷并清除软件运行标记；不检测电机是否已物理停转。 */
void BrushMotor_Init(void) { BrushMotor_Stop(); }
/* 请求低电平启动毛刷，仅控制电机，不自动断开传感器电源。 */
HAL_StatusTypeDef BrushMotor_Start(uint32_t duration_ms)
{
  if (duration_ms == 0U || duration_ms > BOARD_MOTION_MAX_MS) { return HAL_ERROR; }
  if (motor_running) { return HAL_BUSY; }
  /* 运行时禁止刷新时限，避免控制循环使电机永不停机。 */
  started_at = HAL_GetTick();
  duration = duration_ms;
  HAL_GPIO_WritePin(BOARD_BRUSH_MOTOR_PORT, BOARD_BRUSH_MOTOR_PIN, GPIO_PIN_RESET);
  motor_running = true;
  return HAL_OK;
}
/* 无符号时间差兼容系统毫秒计数回绕，主循环到期后关闭输出。 */
void BrushMotor_Task(void)
{
  if (motor_running && (HAL_GetTick() - started_at) >= duration) { BrushMotor_Stop(); }
}
void BrushMotor_Stop(void)
{
  HAL_GPIO_WritePin(BOARD_BRUSH_MOTOR_PORT, BOARD_BRUSH_MOTOR_PIN, GPIO_PIN_SET);
  motor_running = false;
}
/* 返回软件记录的驱动状态，不作为转速或故障反馈。 */
bool BrushMotor_IsRunning(void) { return motor_running; }
/* 返回向上取整的剩余秒数，不延长当前动作。 */
uint32_t BrushMotor_Remaining(void)
{
  uint32_t elapsed=HAL_GetTick()-started_at;
  return !motor_running || elapsed>=duration ? 0U : (duration-elapsed+999U)/1000U;
}
