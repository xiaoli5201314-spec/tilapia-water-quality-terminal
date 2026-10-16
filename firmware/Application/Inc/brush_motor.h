/* 文件：brush_motor.h */
/* 功能：毛刷直流电机启动、停止和动作时长接口。 */
/* 毛刷电机低电平运行，必须指定动作时长。 */
#ifndef __BRUSH_MOTOR_H__
#define __BRUSH_MOTOR_H__
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
/** 使 PB8 输出高电平停止毛刷，不自动执行清洗。 */
void BrushMotor_Init(void);
/** 由主循环周期调用，达到本次运行时限后停止电机。 */
void BrushMotor_Task(void);
/**
 * @brief 请求低电平启动毛刷，仅控制电机，不自动断开传感器电源。
 * @param duration_ms 本次运行时长，单位 ms，范围 1~BOARD_MOTION_MAX_MS。
 * @return HAL_OK 表示已启动；HAL_BUSY 表示已经运行且不会刷新时限；HAL_ERROR 表示时长无效。
 * @note 必须持续调用 BrushMotor_Task 才能执行软件超时停止。
 */
HAL_StatusTypeDef BrushMotor_Start(uint32_t duration_ms);
/** 输出高电平停止毛刷并清除软件运行标记；不检测电机是否已物理停转。 */
void BrushMotor_Stop(void);
/** 返回软件记录的驱动状态，不作为转速或故障反馈。 */
bool BrushMotor_IsRunning(void);
/* 软件通电时限的剩余秒数，不代表实测转速。 */
uint32_t BrushMotor_Remaining(void);
#endif
