/* 文件：water_reporter.c */
/* 功能：实现传感器预热、周期读取、有效性检查和定时发布。 */
/* 整轮采集成功才交付消息，失败时不沿用上一轮的读数。 */
#include "water_reporter.h"
#include "network_config.h"
#include "network_manager.h"
#include "water_sensor.h"
#include "water_report.h"
#include "sensor_power.h"
#include "cleaning_controller.h"
#include "actuator.h"
#include "brush_motor.h"
#include "mqtt_transport.h"
#include "remote_control.h"
#include "stm32f4xx_hal.h"
#include <string.h>

static uint32_t powered_at;
static uint32_t last_sample_at;
static uint32_t last_publish_at;
static bool enabled;
static bool was_powered;
static WaterReading latest;
static bool have_reading;
static bool was_collecting;

/* 暂停通知可在 AT 等待的控制 Poll 中执行，防止短清洗期间遗漏缓存失效。 */
void WaterReporter_Invalidate(void)
{
  have_reading=false;
  was_collecting=false;
}

/* 两路探头上电预热，首次采集必须重新读取，不恢复旧测量缓存。 */
void WaterReporter_Init(void)
{
  SensorPower_AllOn();
  powered_at = HAL_GetTick();
  last_sample_at = powered_at - WATER_SAMPLE_INTERVAL_MS;
  last_publish_at = powered_at - WATER_PUBLISH_INTERVAL_MS;
  enabled = true;
  was_powered = true;
  WaterReporter_Invalidate();
}

/* 暂停/清洗恢复后强制一次新采集，成功前禁止发布任何旧缓存。 */
void WaterReporter_Task(void)
{
  WaterReading reading;
  char topic[80];
  char payload[160];
  MqttLink link;
  uint32_t now = HAL_GetTick();
  const ControlConfig *config=RemoteControl_Config();
  bool powered = SensorPower_IsOn(SENSOR_CHANNEL_1) &&
                 SensorPower_IsOn(SENSOR_CHANNEL_2);
  if (!powered) { was_powered = false; WaterReporter_Invalidate(); return; }
  if (!enabled) { return; }
  if (!was_powered)
  {
    /* 清洗后重新上电时重启预热计时，不能立即把未稳定的数值发布出去。 */
    powered_at = now;
    last_sample_at = now - WATER_SAMPLE_INTERVAL_MS;
    was_powered = true;
    WaterReporter_Invalidate();
    return;
  }
  if (!RemoteControl_CollectionActive() || CleaningController_IsBusy() ||
      Actuator_IsBusy() || BrushMotor_IsRunning())
  { WaterReporter_Invalidate(); return; }
  if ((now-powered_at)<WATER_SENSOR_WARMUP_MS) { return; }
  if (!was_collecting)
  {
    have_reading=false;
    last_sample_at=now-config->sample*1000U;
    was_collecting=true;
  }
  if ((now-last_sample_at)>=config->sample*1000U)
  {
    last_sample_at=now; have_reading=false;
    if (WaterSensor_ReadPh(&reading.ph)!=HAL_OK ||
        WaterSensor_ReadOxygen(&reading.do_mg_l,&reading.water_temp)!=HAL_OK ||
        WaterSensor_ReadSaturation(&reading.do_saturation)!=HAL_OK ||
        !RemoteControl_CollectionActive()) { return; }
    latest=reading; have_reading=true;
  }
  /* 本轮数据有效且到达发布周期才上报；离线时下一轮仍会重新采集。 */
  if (!have_reading || (HAL_GetTick() - last_publish_at) < config->publish*1000U) { return; }
  link = NetworkManager_GetActiveLink();
  if (link == MQTT_LINK_NONE || !WaterReport_BuildTopic(topic, sizeof(topic)) ||
      !WaterReport_BuildPayload(&latest, payload, sizeof(payload))) { return; }
  if (MqttTransport_Publish(link, topic, (const uint8_t *)payload,
                            (uint16_t)strlen(payload)) == MQTT_RESULT_OK)
  { last_publish_at = HAL_GetTick(); }
}
