/* 文件：actuator.h */
/* 功能：推杆双方向控制接口与动作超时保护。 */
/* 推杆电气方向及定时保护；机械伸出方向需首次实机确认。 */
#ifndef __ACTUATOR_H__
#define __ACTUATOR_H__
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
/* STOP 为 00 滑行停止；FORWARD/REVERSE 为电气方向，不是限位反馈。 */
typedef enum { ACTUATOR_STOP = 0, ACTUATOR_FORWARD, ACTUATOR_REVERSE } ActuatorState;
/** 初始化为 00 停止状态，清除待执行动作并开始换向保护计时。 */
void Actuator_Init(void);
/** 由主循环周期调用，处理换向等待和动作超时，不进行阻塞延时。 */
void Actuator_Task(void);
/** 立即输出 00 停止，并取消尚在等待的换向动作；不提供机械锁止。 */
void Actuator_Stop(void);
/**
 * @brief 申请一次定时推杆动作，电气正反转与机械伸缩方向需实机确认。
 * @param direction 只能为 ACTUATOR_FORWARD 或 ACTUATOR_REVERSE；停止请调用 Actuator_Stop。
 * @param duration_ms 实际通电时长，单位 ms，范围 1~BOARD_MOTION_MAX_MS，不含换向等待。
 * @return HAL_OK 表示已接受，可能仍在等待；HAL_BUSY 表示同向运行或已有待执行动作；
 *         HAL_ERROR 表示参数无效。必须持续调用 Actuator_Task 才能执行超时停止。
 */
HAL_StatusTypeDef Actuator_Move(ActuatorState direction, uint32_t duration_ms);
/** 返回当前输出的电气状态；等待换向时返回 ACTUATOR_STOP，不表示没有待执行动作。 */
ActuatorState Actuator_GetState(void);
/** 运行中或等待换向时返回 true；只反映软件动作状态，不检测实际位置或堵转。 */
bool Actuator_IsBusy(void);
/* 软件动作剩余秒数，换向等待不宣称推杆已开始运动。 */
uint32_t Actuator_Remaining(void);
#endif
