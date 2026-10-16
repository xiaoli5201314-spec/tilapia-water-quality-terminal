/* 文件：cleaning_controller.h */
/* 功能：水质传感器自动清洗流程、状态机接口和可调参数。 */
/* 清洗期间传感器保持供电，暂停采集；完成后自动恢复采集和上报。 */
#ifndef __CLEANING_CONTROLLER_H__
#define __CLEANING_CONTROLLER_H__

#include <stdbool.h>
#include "actuator.h"
#include "control_config.h"

/* ======================== 清洗参数配置区 ======================== */
/* 设为 0 可暂时关闭自动清洗，设为 1 时上电后自动执行。 */
#define CLEANING_ENABLED             1U
/* 上电后首次清洗前等待时间；当前设为 60 秒便于首次联调。 */
#define CLEANING_FIRST_DELAY_MS      (60UL * 1000UL)
/* 每次清洗完成后，到下一次清洗开始的间隔；当前为 6 小时。 */
#define CLEANING_INTERVAL_MS         (6UL * 60UL * 60UL * 1000UL)
/* XDHA24-300 理论全行程约 300/30=10 秒，预留到 12 秒。 */
#define CLEANING_EXTEND_TIME_MS      12000U
#define CLEANING_BRUSH_TIME_MS       10000U
#define CLEANING_RETRACT_TIME_MS     12000U
/* 收回后等待传感器机械位置和测量值稳定的时间。 */
#define CLEANING_SETTLE_TIME_MS      30000U

/* 只需修改这两个宏，就能交换推出和收回方向。 */
#define CLEANING_EXTEND_DIRECTION    ACTUATOR_FORWARD
#define CLEANING_RETRACT_DIRECTION   ACTUATOR_REVERSE

typedef enum
{
  CLEANING_NOT_CONFIGURED = 0,
  CLEANING_STARTED,
  CLEANING_BUSY,
  CLEANING_FAILED
} CleaningStartResult;

typedef enum
{
  CLEANING_IDLE = 0,
  CLEANING_EXTENDING,
  CLEANING_BRUSHING,
  CLEANING_RETRACTING,
  CLEANING_SETTLING,
  CLEANING_FAULT
} CleaningState;

/** 初始化清洗状态机，停止机械输出，但不关闭传感器电源。 */
void CleaningController_Init(void);
/** 主循环周期调用，推进推出、刷洗、收回和稳定等待流程。 */
void CleaningController_Task(void);
/** 请求立即开始一次清洗；自动定时和后续远程控制均可复用此接口。 */
CleaningStartResult CleaningController_RequestStart(void);
/** 停止推杆和毛刷并进入故障态；不关闭传感器电源。 */
void CleaningController_RequestStop(void);
/** 清洗动作或清洗后的稳定等待期间返回 true。 */
bool CleaningController_IsBusy(void);
/** 返回当前清洗状态，便于后续界面或调试代码读取。 */
CleaningState CleaningController_GetState(void);
/* 清洗只接受调度器启动，配置在每次开始时冻结；取消可回到空闲。 */
void CleaningController_Configure(const ControlCleaning *config);
void CleaningController_Cancel(void);

#endif
