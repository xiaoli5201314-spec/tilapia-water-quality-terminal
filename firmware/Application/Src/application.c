/* 文件：application.c */
/* 功能：组织所有业务模块的初始化、周期任务和紧急停止。 */
/* 业务入口调度 Wi-Fi、采集、自动清洗与机械保护；4G 暂不参与业务链路。 */
#include "application.h"
#include "status_led.h"
#include "alarm_light.h"
#include "relay_control.h"
#include "brush_motor.h"
#include "actuator.h"
#include "sensor_power.h"
#include "uart_channel.h"
#include "rs485.h"
#include "water_sensor.h"
#include "esp12f_wifi.h"
#include "air780_4g.h"
#include "mqtt_transport.h"
#include "network_manager.h"
#include "cleaning_controller.h"
#include "water_reporter.h"
#include "at_command.h"
#include "control_clock.h"
#include "remote_control.h"
static uint32_t heartbeat_at;  /* 上一次 RUN 翻转的毫秒时刻，不表示采集或上传时间。 */
static bool heartbeat_on;      /* RUN 心跳亮灭状态，仅说明主循环仍在执行。 */

/* CubeMX 初始化后启用串口、传感器供电、Wi-Fi 链路和 RUN 心跳。 */
void Application_Init(void)
{
  StatusLed_Init();
  AlarmLight_Init();
  RelayControl_Init();
  BrushMotor_Init();
  Actuator_Init();
  SensorPower_Init();
  Rs485_Init();
  WaterSensor_Init();
  Esp12fWifi_Init();
  Air780_4G_Init();
  MqttTransport_Init();
  NetworkManager_Init();
  CleaningController_Init();
  ControlClock_Init();
  RemoteControl_Init();
  AtCommand_Init();
  /* 串口中断接收失败时亮 ERR；RUN 每半秒翻转，表示主循环存活。 */
  if (UartChannel_Init() == HAL_OK)
  {
    StatusLed_SetError(false);
    WaterReporter_Init();
  }
  else { StatusLed_SetError(true); }
  heartbeat_at = HAL_GetTick();
  heartbeat_on = true;
  StatusLed_SetRun(heartbeat_on);
}

/* AT 等待期间由事务层继续维护电机超时；采集仅在执行器停止时进行。 */
void Application_Task(void)
{
  UartChannel_Task();
  AtCommand_Poll();
  Actuator_Task();
  BrushMotor_Task();
  NetworkManager_Task();
  ControlClock_Task();
  RemoteControl_Poll();
  RemoteControl_PersistTask();
  RemoteControl_StatusTask();
  WaterReporter_Task();
  if ((HAL_GetTick() - heartbeat_at) >= 500U)
  {
    heartbeat_at = HAL_GetTick();
    heartbeat_on = !heartbeat_on;
    StatusLed_SetRun(heartbeat_on);
  }
}

/* 停止推杆和毛刷、释放继电器并亮红灯/ERR；按当前需求不切断传感器电源。 */
void Application_EmergencyStop(void)
{
  RemoteControl_EmergencyStop();
  RelayControl_AllOff();
  AlarmLight_Set(ALARM_LIGHT_RED);
  StatusLed_SetError(true);
}
