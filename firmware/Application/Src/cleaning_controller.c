/* 文件：cleaning_controller.c */
/* 功能：推杆推出、毛刷清洗、推杆收回的非阻塞状态机。 */
/* 清洗期间保持传感器供电，只暂停水质读取；所有动作均有软件时限保护。 */
#include "cleaning_controller.h"
#include "actuator.h"
#include "brush_motor.h"
#include "alarm_light.h"
#include "status_led.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>

static CleaningState state;
static uint32_t phase_started_at;
static ControlCleaning configured, running;

/* 使用有符号差值判断到期，兼容 HAL_GetTick() 回绕。 */
/* 冻结下一次流程配置，运行中的阶段参数不受远程更新影响。 */
void CleaningController_Configure(const ControlCleaning *config)
{
  if (config!=NULL) { configured=*config; }
}

/* 清洗动作失败时立即关闭机械输出，并通过红灯和 ERR 指示故障。 */
static void EnterFault(void)
{
  Actuator_Stop();
  BrushMotor_Stop();
  state = CLEANING_FAULT;
  AlarmLight_Set(ALARM_LIGHT_RED);
  StatusLed_SetError(true);
}

/* 初始化清洗状态机：停止机械输出，但保持传感器电源策略不变。 */
void CleaningController_Init(void)
{
  ControlConfig defaults;
  uint32_t now = HAL_GetTick();
  Actuator_Stop();
  BrushMotor_Stop();
  state = CLEANING_IDLE;
  phase_started_at = now;
  ControlConfig_Default(&defaults);
  configured=defaults.cleaning;
  AlarmLight_Set(ALARM_LIGHT_GREEN);
  StatusLed_SetError(false);
}

/* 请求一次清洗：先推出，推出完成后由 Task() 自动进入刷洗阶段。 */
CleaningStartResult CleaningController_RequestStart(void)
{
  if (state == CLEANING_FAULT) { return CLEANING_FAILED; }
  if (state != CLEANING_IDLE || Actuator_IsBusy() || BrushMotor_IsRunning()) { return CLEANING_BUSY; }
  running=configured;
  if (Actuator_Move(CLEANING_EXTEND_DIRECTION, running.extend*1000U) != HAL_OK)
  {
    EnterFault();
    return CLEANING_FAILED;
  }
  state = CLEANING_EXTENDING;
  phase_started_at = HAL_GetTick();
  AlarmLight_Set(ALARM_LIGHT_YELLOW);
  return CLEANING_STARTED;
}

/* 非阻塞推进清洗时序，不在这里使用长时间 HAL_Delay。 */
void CleaningController_Task(void)
{
  uint32_t now = HAL_GetTick();
  if (state == CLEANING_FAULT) { return; }

  switch (state)
  {
    case CLEANING_IDLE:
      break;

    case CLEANING_EXTENDING:
      /* 推杆内部到达端点会自停；软件也会在设定时间后停止输出。 */
      if (!Actuator_IsBusy())
      {
        if (BrushMotor_Start(running.brush*1000U) != HAL_OK)
        {
          EnterFault();
          break;
        }
        state = CLEANING_BRUSHING;
        phase_started_at = now;
      }
      break;

    case CLEANING_BRUSHING:
      if (!BrushMotor_IsRunning())
      {
        if (Actuator_Move(CLEANING_RETRACT_DIRECTION, running.retract*1000U) != HAL_OK)
        {
          EnterFault();
          break;
        }
        state = CLEANING_RETRACTING;
        phase_started_at = now;
      }
      break;

    case CLEANING_RETRACTING:
      /* 这里只知道输出计时已停止，不宣称推杆已到达实测位置。 */
      if (!Actuator_IsBusy())
      {
        state = CLEANING_SETTLING;
        phase_started_at = now;
      }
      break;

    case CLEANING_SETTLING:
      if ((now - phase_started_at) >= running.settle*1000U)
      {
        state = CLEANING_IDLE;
        AlarmLight_Set(ALARM_LIGHT_GREEN);
      }
      break;

    default:
      EnterFault();
      break;
  }
}

/* 停止请求只停止执行器，不切断传感器供电。 */
void CleaningController_RequestStop(void)
{
  Actuator_Stop();
  BrushMotor_Stop();
  state = CLEANING_FAULT;
  AlarmLight_Set(ALARM_LIGHT_RED);
  StatusLed_SetError(true);
}

/* 稳定等待也算清洗占用，防止传感器尚未稳定就开始读取。 */
bool CleaningController_IsBusy(void)
{
  return state != CLEANING_IDLE;
}

CleaningState CleaningController_GetState(void)
{
  return state;
}
/* 远程取消进入可恢复空闲，故障接口仍保留红灯与 ERR。 */
void CleaningController_Cancel(void)
{
  Actuator_Stop(); BrushMotor_Stop();
  state=CLEANING_IDLE; AlarmLight_Set(ALARM_LIGHT_GREEN);
}
