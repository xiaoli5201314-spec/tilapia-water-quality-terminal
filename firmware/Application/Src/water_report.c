/* 文件：water_report.c */
/* 功能：实现基于 STM32 唯一 ID 的 Topic 和四字段水质 JSON 生成。 */
/* 只转换已采集成功的水质数据；构造过程不访问网络。 */
#include "water_report.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>

bool WaterReport_BuildTopic(char *topic, uint16_t capacity)
{
  int length;
  if (topic == NULL || capacity == 0U) { return false; }
  topic[0] = '\0';
  length = snprintf(topic, capacity, "fish/device/%08lX%08lX%08lX/water/latest",
    (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
    (unsigned long)HAL_GetUIDw2());
  if (length < 0 || length >= capacity) { topic[0] = '\0'; return false; }
  return true;
}

bool WaterReport_BuildPayload(const WaterReading *reading, char *payload, uint16_t capacity)
{
  int length;
  if (payload == NULL || capacity == 0U) { return false; }
  payload[0] = '\0';
  /* 不允许 NaN/无穷大或损坏的寄存器数值变成 JSON 中的有效测量。 */
  if (reading == NULL ||
      !(reading->ph >= 0.0F && reading->ph <= 14.0F) ||
      !(reading->do_mg_l >= 0.0F && reading->do_mg_l <= 100.0F) ||
      !(reading->water_temp >= -50.0F && reading->water_temp <= 120.0F) ||
      !(reading->do_saturation >= 0.0F && reading->do_saturation <= 300.0F))
  { return false; }
  length = snprintf(payload, capacity,
    "{\"ph\":%.2f,\"doMgL\":%.2f,\"waterTemp\":%.2f,\"doSaturation\":%.2f}",
    (double)reading->ph, (double)reading->do_mg_l,
    (double)reading->water_temp, (double)reading->do_saturation);
  if (length < 0 || length >= capacity) { payload[0] = '\0'; return false; }
  return true;
}
