/* 文件：status_led.h */
/* 功能：板载 RUN 和 ERR 状态指示灯控制接口。 */
/* 板载 RUN/ERR 状态灯，均为低电平点亮。 */
#ifndef __STATUS_LED_H__
#define __STATUS_LED_H__

#include <stdbool.h>

/** 初始化时将 RUN、ERR 都置高熄灭。 */
void StatusLed_Init(void);
/** on=true 时 RUN 低电平点亮；主循环心跳会周期更新此状态。 */
void StatusLed_SetRun(bool on);
/** on=true 时 ERR 低电平点亮，false 熄灭；该接口不自行判定故障。 */
void StatusLed_SetError(bool on);

#endif /* __STATUS_LED_H__ */
