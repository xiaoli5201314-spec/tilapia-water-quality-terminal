/* 文件：alarm_light.h */
/* 功能：红、黄、绿三色报警灯控制接口。 */
/* 三色灯：PB5 红、PB4 黄、PB3 绿，同一时间只亮一种颜色。 */
#ifndef __ALARM_LIGHT_H__
#define __ALARM_LIGHT_H__

typedef enum
{
  ALARM_LIGHT_OFF = 0,
  ALARM_LIGHT_RED,
  ALARM_LIGHT_YELLOW,
  ALARM_LIGHT_GREEN
} AlarmLightColor;

/** 初始化为三色全灭，不默认宣告水质或网络正常。 */
void AlarmLight_Init(void);
/** 选择 color 对应的一种灯色，其他灯熄灭；OFF 或无效值使三色全灭，低电平点亮。 */
void AlarmLight_Set(AlarmLightColor color);
/** 返回上次设置的灯色，不检测外接灯具是否实际点亮。 */
AlarmLightColor AlarmLight_Get(void);

#endif /* __ALARM_LIGHT_H__ */
