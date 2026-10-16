/* 水质消息格式及设备身份测试，不连接网络或传感器。 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "water_report.h"

uint32_t HAL_GetUIDw0(void) { return 0x01234567U; }
uint32_t HAL_GetUIDw1(void) { return 0x89ABCDEFU; }
uint32_t HAL_GetUIDw2(void) { return 0x13572468U; }

int main(void)
{
  char topic[80];
  char payload[160];
  WaterReading reading = {7.12F, 8.68F, 22.52F, 96.5F};
  assert(WaterReport_BuildTopic(topic, sizeof(topic)));
  assert(strcmp(topic, "fish/device/0123456789ABCDEF13572468/water/latest") == 0);
  assert(!WaterReport_BuildTopic(topic, 8U));
  assert(WaterReport_BuildPayload(&reading, payload, sizeof(payload)));
  assert(strcmp(payload,
    "{\"ph\":7.12,\"doMgL\":8.68,\"waterTemp\":22.52,\"doSaturation\":96.50}") == 0);
  assert(!WaterReport_BuildPayload(&reading, payload, 8U));
  reading.ph = NAN;
  assert(!WaterReport_BuildPayload(&reading, payload, sizeof(payload)));
  puts("PASS: unique topic, four measured values, invalid payload rejection");
  return 0;
}
