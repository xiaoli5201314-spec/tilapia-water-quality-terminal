/* 文件：actuator.c */
/* 功能：实现推杆正转、反转、停止、换向保护和超时停止。 */
/* DRV8870 推杆控制，不自动执行任何伸缩动作。 */
#include "actuator.h"
#include "board_config.h"
static ActuatorState current_state;  /* 当前已经施加到驱动输入的方向。 */
static ActuatorState pending_state;  /* 等待换向保护结束的方向，STOP 表示无待执行动作。 */
static uint32_t movement_started;    /* 实际开始通电的毫秒时刻，不含换向等待。 */
static uint32_t movement_duration;   /* 本次动作允许持续的毫秒数。 */
static uint32_t stopped_at;          /* 最近一次从运行转为停止的时刻，用于换向保护。 */

/* 将电气方向映射到 DRV8870 输入；先清零两脚，避免换向瞬间进入 11 制动状态。 */
static void Actuator_Write(ActuatorState state)
{
  /* 先清零两个输入，避免换向过程中短暂进入 11 制动状态。 */
  HAL_GPIO_WritePin(BOARD_ACTUATOR_IN1_PORT, BOARD_ACTUATOR_IN1_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(BOARD_ACTUATOR_IN2_PORT, BOARD_ACTUATOR_IN2_PIN, GPIO_PIN_RESET);
  if (state == ACTUATOR_FORWARD)
  {
    HAL_GPIO_WritePin(BOARD_ACTUATOR_IN1_PORT, BOARD_ACTUATOR_IN1_PIN, BOARD_ACTUATOR_FORWARD_IN1);
    HAL_GPIO_WritePin(BOARD_ACTUATOR_IN2_PORT, BOARD_ACTUATOR_IN2_PIN, BOARD_ACTUATOR_FORWARD_IN2);
  }
  else if (state == ACTUATOR_REVERSE)
  {
    HAL_GPIO_WritePin(BOARD_ACTUATOR_IN1_PORT, BOARD_ACTUATOR_IN1_PIN, BOARD_ACTUATOR_REVERSE_IN1);
    HAL_GPIO_WritePin(BOARD_ACTUATOR_IN2_PORT, BOARD_ACTUATOR_IN2_PIN, BOARD_ACTUATOR_REVERSE_IN2);
  }
}

/* 初始化为 00 停止状态，清除待执行动作并开始换向保护计时。 */
void Actuator_Init(void)
{
  current_state = ACTUATOR_STOP;
  pending_state = ACTUATOR_STOP;
  Actuator_Stop();
  stopped_at = HAL_GetTick();
}

/* 立即输出 00 停止，并取消尚在等待的换向动作；不提供机械锁止。 */
void Actuator_Stop(void)
{
  if (current_state != ACTUATOR_STOP) { stopped_at = HAL_GetTick(); }
  Actuator_Write(ACTUATOR_STOP);
  current_state = ACTUATOR_STOP;
  pending_state = ACTUATOR_STOP;
}

/* 申请一次定时推杆动作，电气正反转与机械伸缩方向需实机确认。 */
HAL_StatusTypeDef Actuator_Move(ActuatorState direction, uint32_t duration_ms)
{
  if ((direction != ACTUATOR_FORWARD && direction != ACTUATOR_REVERSE) ||
      duration_ms == 0U || duration_ms > BOARD_MOTION_MAX_MS) { return HAL_ERROR; }
  /* 不允许重复调用不断延长同一次动作的保护时长。 */
  if (current_state == direction || pending_state != ACTUATOR_STOP) { return HAL_BUSY; }
  Actuator_Stop();
  pending_state = direction;
  movement_duration = duration_ms;
  Actuator_Task();
  return HAL_OK;
}

/* 非阻塞维护换向等待和运行时限，时间差按无符号毫秒计算以兼容计数回绕。 */
void Actuator_Task(void)
{
  uint32_t now = HAL_GetTick();
  if (pending_state != ACTUATOR_STOP &&
      (now - stopped_at) >= BOARD_REVERSAL_GUARD_MS)
  {
    current_state = pending_state;
    pending_state = ACTUATOR_STOP;
    movement_started = now;
    Actuator_Write(current_state);
  }
  if (current_state != ACTUATOR_STOP &&
      (now - movement_started) >= movement_duration) { Actuator_Stop(); }
}

/* 返回当前输出的电气状态；等待换向时返回 ACTUATOR_STOP，不表示没有待执行动作。 */
ActuatorState Actuator_GetState(void) { return current_state; }
/* 运行中或等待换向时返回 true；只反映软件动作状态，不检测实际位置或堵转。 */
bool Actuator_IsBusy(void)
{
  return current_state != ACTUATOR_STOP || pending_state != ACTUATOR_STOP;
}
/* 只报告软件定时剩余，不推断行程位置或限位。 */
uint32_t Actuator_Remaining(void)
{
  uint32_t elapsed;
  if (pending_state!=ACTUATOR_STOP) { return (movement_duration+999U)/1000U; }
  if (current_state==ACTUATOR_STOP) { return 0U; }
  elapsed=HAL_GetTick()-movement_started;
  return elapsed>=movement_duration ? 0U : (movement_duration-elapsed+999U)/1000U;
}
