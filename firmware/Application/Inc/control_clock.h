/* 文件：control_clock.h */
/* 功能：ESP SNTP 北京时区校时、UTC 秒时钟及同步失效控制。 */
#ifndef CONTROL_CLOCK_H
#define CONTROL_CLOCK_H
#include <stdint.h>
#include <stdbool.h>
void ControlClock_Init(void);
/* 解析完整英文星期/月历，输入为北京时间，输出 UTC Unix 秒。 */
bool ControlClock_Parse(const char *text, uint32_t *unix_time);
void ControlClock_SetUnix(uint32_t unix_time);
bool ControlClock_IsSynced(void);
uint32_t ControlClock_Unix(void);
uint32_t ControlClock_Generation(void);
/* 只在可用 Wi-Fi 前台发 AT，等待期间仍执行机械与命令保护。 */
void ControlClock_Task(void);
void ControlClock_LinkChanged(bool connected);
#endif
