/* 文件：water_report.h */
/* 功能：生成设备唯一 Topic 和四字段水质 JSON。 */
/* 新成品板只上报四项水质读数，主题中的芯片 ID 用于区分设备。 */
#ifndef WATER_REPORT_H
#define WATER_REPORT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  float ph;
  float do_mg_l;
  float water_temp;
  float do_saturation;
} WaterReading;

/** 用 STM32 出厂唯一 ID 生成不含鱼塘编号的 MQTT 主题。 */
bool WaterReport_BuildTopic(char *topic, uint16_t capacity);
/** 所有读数有效时生成四字段 JSON；失败时输出为空串，不能发布。 */
bool WaterReport_BuildPayload(const WaterReading *reading, char *payload, uint16_t capacity);
#endif
