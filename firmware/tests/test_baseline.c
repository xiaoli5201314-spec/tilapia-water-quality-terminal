/* 逻辑单元测试：使用假 HAL 验证真实 Application 源码，不代替实机联调。 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board_config.h"
#include "application.h"
#include "gpio.h"
#include "actuator.h"
#include "brush_motor.h"
#include "alarm_light.h"
#include "relay_control.h"
#include "sensor_power.h"
#include "uart_channel.h"
#include "at_command.h"
#include "modbus_rtu.h"
#include "water_sensor.h"
#include "rs485.h"
#include "esp12f_wifi.h"
#include "air780_4g.h"
#include "network_manager.h"
#include "cleaning_controller.h"
#include "water_reporter.h"
#include "control_clock.h"
#include "remote_control.h"

GPIO_TypeDef test_gpio_a, test_gpio_b, test_gpio_c, test_gpio_d;
GPIO_TypeDef test_gpio_e, test_gpio_g, test_gpio_h;
UART_HandleTypeDef huart1, huart2, huart3;
static uint32_t tick;
static uint32_t irq_mask;
static const uint8_t *reply;
static uint16_t reply_length;
static HAL_StatusTypeDef tx_result;
static uint32_t tx_tick;
static unsigned int tx_count;
static uint8_t request_copy[256];
static unsigned int network_mock;
static unsigned int wifi_up = 1U;
static unsigned int cell_up = 1U;
static unsigned int cell_publishes;
static unsigned int wifi_publishes;
static unsigned int sensor_mock;
static unsigned int cell_socket_open;
static unsigned int cell_fail_connect_once;
static unsigned int wifi_id_seen;
static unsigned int cell_id_seen;
static unsigned int sensor_queries;
static uint16_t raw_remaining, raw_used;
static uint8_t raw_copy[512];
static unsigned int raw_success = 1U;
/* 模拟真实模块的延迟发布应答和跨 STM32 重启保留的 MQTT 会话。 */
static uint32_t raw_ack_delay, raw_ack_at;
static unsigned int raw_ack_pending, network_session_mock, wifi_mqtt_connected;
static unsigned int wifi_clean_empty_error;
static uint32_t raw_prompt_delay, raw_prompt_at, raw_tx_cost;
static unsigned int raw_prompt_pending;
static void (*delay_hook)(void);

uint32_t HAL_GetTick(void) { return tick; }
uint32_t HAL_GetUIDw0(void) { return 0x01234567U; }
uint32_t HAL_GetUIDw1(void) { return 0x89ABCDEFU; }
uint32_t HAL_GetUIDw2(void) { return 0x13572468U; }
void HAL_Delay(uint32_t delay) { tick += delay; if (delay_hook!=NULL) { delay_hook(); } }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
  if (state == GPIO_PIN_SET) { port->pins |= pin; }
  else { port->pins &= (uint16_t)~pin; }
}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{ return (port->pins & pin) != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET; }
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *config) { (void)port; (void)config; }
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *uart, uint8_t *byte, uint16_t length)
{
  assert(length == 1U);
  uart->rx_byte = byte;
  uart->RxState = HAL_UART_STATE_BUSY_RX;
  return HAL_OK;
}
static void InjectByte(UART_HandleTypeDef *uart, uint8_t byte)
{
  assert(uart->rx_byte != NULL);
  *uart->rx_byte = byte;
  uart->RxState = HAL_UART_STATE_READY;
  HAL_UART_RxCpltCallback(uart);
}
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart, uint8_t *bytes,
                                  uint16_t length, uint32_t timeout)
{
  uint16_t i;
  (void)timeout;
  assert(length < sizeof(request_copy));
  memcpy(request_copy, bytes, length);
  request_copy[length] = '\0';
  ++tx_count;
  tx_tick = tick;
  if (uart==&huart1 && raw_remaining!=0U)
  {
    assert(length<=raw_remaining && raw_used+length<sizeof(raw_copy));
    memcpy(raw_copy+raw_used,bytes,length); raw_used=(uint16_t)(raw_used+length);
    raw_remaining=(uint16_t)(raw_remaining-length);
    tick+=raw_tx_cost;
    if (raw_remaining==0U)
    {
      const char *answer=raw_success ? "+MQTTPUB:OK\r\n" : "OK\r\n";
      raw_copy[raw_used]='\0';
      if (raw_ack_delay!=0U && raw_success)
      { raw_ack_at=tick+raw_ack_delay; raw_ack_pending=1U; }
      else
      { for (i=0U;answer[i]!='\0';++i) { InjectByte(uart,(uint8_t)answer[i]); } }
    }
    return HAL_OK;
  }
  if (sensor_mock != 0U && uart == &huart3)
  {
    uint8_t frame[16];
    uint16_t frame_length;
    uint16_t crc;
    ++sensor_queries;
    if (bytes[0] == 1U)
    {
      const uint8_t ph[] = {1U, 3U, 2U, 2U, 0xC8U};
      memcpy(frame, ph, sizeof(ph));
      frame_length = 7U;
    }
    else if (bytes[2] == 0U && bytes[3] == 4U)
    {
      const uint8_t saturation[] = {2U, 3U, 4U, 3U, 0xC5U, 0U, 1U};
      memcpy(frame, saturation, sizeof(saturation));
      frame_length = 9U;
    }
    else
    {
      const uint8_t oxygen[] = {2U, 3U, 8U, 3U, 0x64U, 0U, 2U, 8U, 0xCCU, 0U, 2U};
      memcpy(frame, oxygen, sizeof(oxygen));
      frame_length = 13U;
    }
    crc = ModbusRtu_Crc16(frame, (uint16_t)(frame_length - 2U));
    frame[frame_length - 2U] = (uint8_t)crc;
    frame[frame_length - 1U] = (uint8_t)(crc >> 8U);
    for (i = 0U; i < frame_length; ++i) { InjectByte(uart, frame[i]); }
    return HAL_OK;
  }
  if (network_mock != 0U && (uart == &huart1 || uart == &huart2))
  {
    const char *answer = "OK\r\n";
    if ((uart == &huart1 && !wifi_up) || (uart == &huart2 && !cell_up))
    { answer = "ERROR\r\n"; }
    else if (strstr((char *)request_copy, "AT+MQTTCONN?") != NULL)
    {
      answer = network_session_mock && !wifi_mqtt_connected ?
        "+MQTTCONN:0,1\r\nOK\r\n" :
        "+MQTTCONN:0,4,1,\"broker\",1883,\"\",0\r\nOK\r\n";
    }
    else if (strstr((char *)request_copy, "AT+MQTTCLEAN=0") != NULL)
    {
      answer=wifi_clean_empty_error && !wifi_mqtt_connected ? "ERROR\r\n" :
        "+MQTTDISCONNECTED:0\r\nOK\r\n";
      wifi_mqtt_connected=0U;
    }
    else if (strstr((char *)request_copy, "AT+MQTTCONN=") != NULL &&
             network_session_mock)
    {
      answer=wifi_mqtt_connected ? "ERROR\r\n" : "OK\r\n";
      wifi_mqtt_connected=1U;
    }
    else if (strstr((char *)request_copy, "AT+CIPSNTPTIME?") != NULL)
    { answer = "+CIPSNTPTIME:Mon Oct 12 12:33:20 2026\r\nOK\r\n"; }
    else if (strstr((char *)request_copy, "AT+CPIN?") != NULL)
    { answer = "+CPIN: READY\r\nOK\r\n"; }
    else if (strstr((char *)request_copy, "AT+CGATT?") != NULL)
    { answer = "+CGATT: 1\r\nOK\r\n"; }
    else if (strstr((char *)request_copy, "AT+MIPSTART=") != NULL)
    {
      answer = cell_socket_open ? "OK\r\nALREADY CONNECT\r\n" : "OK\r\nCONNECT OK\r\n";
      cell_socket_open = 1U;
    }
    else if (strstr((char *)request_copy, "AT+MCONNECT=") != NULL)
    {
      answer = cell_fail_connect_once ? "ERROR\r\n" : "OK\r\nCONNACK OK\r\n";
      cell_fail_connect_once = 0U;
    }
    else if (strstr((char *)request_copy, "AT+MIPCLOSE") != NULL)
    { cell_socket_open = 0U; }
    else if (strstr((char *)request_copy, "AT+MQTTSTATU") != NULL)
    { answer = "+MQTTSTATU: 1\r\nOK\r\n"; }
    if (uart == &huart2 && strstr((char *)request_copy, "AT+MPUB=") != NULL)
    { ++cell_publishes; }
    if (uart == &huart1 && strstr((char *)request_copy, "AT+MQTTUSERCFG=") != NULL &&
        strstr((char *)request_copy, "fish-0123456789ABCDEF13572468-w") != NULL)
    { ++wifi_id_seen; }
    if (uart == &huart2 && strstr((char *)request_copy, "AT+MCONFIG=") != NULL &&
        strstr((char *)request_copy, "fish-0123456789ABCDEF13572468-c") != NULL)
    { ++cell_id_seen; }
    if (uart == &huart1 && strncmp((char *)request_copy, "AT+MQTTPUBRAW=",13U)==0)
    {
      char *end=strrchr((char *)request_copy,'"');
      unsigned int amount=0U;
      assert(end!=NULL && sscanf(end+1,",%u,0,1",&amount)==1 && amount<512U);
      raw_remaining=(uint16_t)amount; raw_used=0U;
      ++wifi_publishes;
      if (raw_prompt_delay!=0U)
      { raw_prompt_pending=1U; raw_prompt_at=tick+raw_prompt_delay; answer="OK\r\n"; }
      else { answer="OK\r\n>"; }
    }
    for (i = 0U; answer[i] != '\0'; ++i) { InjectByte(uart, (uint8_t)answer[i]); }
    return HAL_OK;
  }
  /* 模块在发送返回前就应答，验证中断缓冲不会丢失立即应答。 */
  if (tx_result == HAL_OK) {
    for (i = 0U; i < reply_length; ++i) { InjectByte(uart, reply[i]); }
  }
  return tx_result;
}
uint32_t __get_PRIMASK(void) { return irq_mask; }
void __disable_irq(void) { irq_mask = 1U; }
void __set_PRIMASK(uint32_t mask) { irq_mask = mask; }

static void SetReply(const uint8_t *bytes, uint16_t length)
{ reply = bytes; reply_length = length; tx_result = HAL_OK; }
static void AppendCrc(uint8_t *frame, uint16_t length)
{
  uint16_t crc = ModbusRtu_Crc16(frame, (uint16_t)(length - 2U));
  frame[length - 2U] = (uint8_t)crc;
  frame[length - 1U] = (uint8_t)(crc >> 8U);
}
static void TestOutputs(void)
{
  uint16_t pins;
  /* 直接执行 CubeMX 生成的 GPIO 初始化，验证启动电平而不只测自写模块。 */
  MX_GPIO_Init();
  assert((GPIOB->pins & (GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_8)) ==
         (GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_8));
  assert((GPIOB->pins & (GPIO_PIN_6 | GPIO_PIN_7)) == 0U);
  assert((GPIOD->pins & (GPIO_PIN_10 | GPIO_PIN_11)) == (GPIO_PIN_10 | GPIO_PIN_11));
  assert((GPIOG->pins & (GPIO_PIN_12 | GPIO_PIN_13)) == (GPIO_PIN_12 | GPIO_PIN_13));
  Application_Init();
  assert(!Actuator_IsBusy() && !BrushMotor_IsRunning());
  assert(SensorPower_IsOn(SENSOR_CHANNEL_1) && SensorPower_IsOn(SENSOR_CHANNEL_2));
  AlarmLight_Set(ALARM_LIGHT_RED);
  assert((GPIOB->pins & GPIO_PIN_5) == 0U && (GPIOB->pins & GPIO_PIN_3) != 0U);
  AlarmLight_Set(ALARM_LIGHT_GREEN);
  assert((GPIOB->pins & GPIO_PIN_3) == 0U && (GPIOB->pins & GPIO_PIN_5) != 0U);
  AlarmLight_Set((AlarmLightColor)99);
  assert(AlarmLight_Get() == ALARM_LIGHT_OFF);
  SensorPower_Set(SENSOR_CHANNEL_1, true);
  SensorPower_Set(SENSOR_CHANNEL_2, false);
  assert((GPIOD->pins & GPIO_PIN_11) == 0U && (GPIOD->pins & GPIO_PIN_10) != 0U);
  pins = GPIOD->pins;
  SensorPower_Set((SensorPowerChannel)42, true);
  assert(GPIOD->pins == pins && !SensorPower_IsOn((SensorPowerChannel)42));
  RelayControl_Set(RELAY_CHANNEL_1, true);
  assert((GPIOG->pins & GPIO_PIN_13) == 0U);
  RelayControl_AllOff();
  SensorPower_AllOff();
  assert(CleaningController_RequestStart() == CLEANING_STARTED);
  CleaningController_Cancel();
  GPIOD->pins |= GPIO_PIN_4;
  assert(Air780_4G_ReadRdy() && !Air780_4G_IsReady());
  NetworkManager_Task();
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_NONE);
  assert(MqttTransport_Publish(MQTT_LINK_WIFI, "test", (const uint8_t *)"{}", 2U) ==
         MQTT_RESULT_NOT_CONNECTED);
  assert(MqttTransport_Publish(MQTT_LINK_NONE, NULL, NULL, 0U) == MQTT_RESULT_INVALID_ARGUMENT);
  puts("PASS: CubeMX safe outputs, mapping, invalid channels, RDY/MQTT separation");
}

static void TestMotion(void)
{
  char response[16];
  tick = 1000U;
  Actuator_Init();
  tick += BOARD_REVERSAL_GUARD_MS;
  assert(Actuator_Move(ACTUATOR_FORWARD, 1000U) == HAL_OK);
  assert((GPIOB->pins & (GPIO_PIN_6 | GPIO_PIN_7)) == GPIO_PIN_6);
  assert(Actuator_Move(ACTUATOR_FORWARD, 1000U) == HAL_BUSY);
  tick += 1U;
  assert(Actuator_Move(ACTUATOR_REVERSE, 200U) == HAL_OK);
  assert((GPIOB->pins & (GPIO_PIN_6 | GPIO_PIN_7)) == 0U && Actuator_IsBusy());
  SetReply((const uint8_t *)"OK\r\n", 4U);
  assert(Esp12fWifi_SendCommand("AT", response, sizeof(response), 10U) == HAL_OK);
  tick += 99U; Actuator_Task(); assert(Actuator_GetState() == ACTUATOR_STOP);
  tick += 1U; Actuator_Task();
  assert((GPIOB->pins & (GPIO_PIN_6 | GPIO_PIN_7)) == GPIO_PIN_7);
  tick += 200U; Actuator_Task(); assert(!Actuator_IsBusy());
  assert(Actuator_Move(ACTUATOR_FORWARD, 0U) == HAL_ERROR);
  assert(Actuator_Move(ACTUATOR_FORWARD, BOARD_MOTION_MAX_MS + 1U) == HAL_ERROR);
  assert(BrushMotor_Start(200U) == HAL_OK);
  assert(BrushMotor_Start(200U) == HAL_BUSY);
  tick += 200U; BrushMotor_Task(); assert(!BrushMotor_IsRunning());
  tick = UINT32_MAX - 20U;
  assert(BrushMotor_Start(50U) == HAL_OK);
  tick += 50U; BrushMotor_Task(); assert(!BrushMotor_IsRunning());
  tick = 5000U;
  assert(Actuator_Move(ACTUATOR_FORWARD, 100U) == HAL_OK);
  Application_EmergencyStop();
  assert(!Actuator_IsBusy() && !BrushMotor_IsRunning());
  assert((GPIOB->pins & (GPIO_PIN_6 | GPIO_PIN_7)) == 0U);
  puts("PASS: motor time limits, reversal gap, rollover, emergency stop");
}

static void TestProtocol(void)
{
  uint8_t frame[8];
  uint8_t valid[] = {1U, 3U, 2U, 2U, 0xAEU, 0x38U, 0x98U};
  uint8_t exception[] = {1U, 0x83U, 2U, 0U, 0U};
  uint8_t odd[] = {1U, 3U, 1U, 0U, 0U, 0U};
  uint8_t code = 0U;
  uint8_t payload_length;
  float ph = 0.0F, oxygen = 0.0F, temp = 0.0F, saturation = 0.0F;
  uint8_t oxygen_payload[] = {3U, 0x64U, 0U, 2U, 8U, 0xCCU, 0U, 2U};
  uint8_t sat_payload[] = {3U, 0xC5U, 0U, 1U};
  assert(ModbusRtu_BuildReadHoldingRegisters(1U, 0U, 1U, frame, sizeof(frame)) == 8U);
  assert(frame[6] == 0x84U && frame[7] == 0x0AU);
  assert(ModbusRtu_BuildReadHoldingRegisters(0U, 0U, 1U, frame, 8U) == 0U);
  assert(ModbusRtu_BuildReadHoldingRegisters(248U, 0U, 1U, frame, 8U) == 0U);
  assert(ModbusRtu_BuildReadHoldingRegisters(1U, 65535U, 2U, frame, 8U) == 0U);
  assert(ModbusRtu_BuildReadHoldingRegisters(1U, 0U, 126U, frame, 8U) == 0U);
  assert(ModbusRtu_BuildReadHoldingRegisters(1U, 0U, 1U, frame, 7U) == 0U);
  assert(ModbusRtu_ValidateReadResponse(valid, sizeof(valid), 1U, 1U, &code) == MODBUS_RESULT_OK);
  assert(ModbusRtu_ValidateReadResponse(valid, sizeof(valid), 1U, 2U, &code) == MODBUS_RESULT_INVALID);
  assert(ModbusRtu_ValidateReadResponse(valid, sizeof(valid), 2U, 1U, &code) == MODBUS_RESULT_INVALID);
  AppendCrc(exception, sizeof(exception));
  assert(ModbusRtu_ValidateReadResponse(exception, sizeof(exception), 1U, 1U, &code) == MODBUS_RESULT_EXCEPTION);
  assert(code == 2U);
  AppendCrc(odd, sizeof(odd));
  assert(ModbusRtu_ValidateResponse(odd, sizeof(odd), 1U, 3U, &payload_length) == HAL_ERROR);
  assert(payload_length == 0U);
  valid[6] ^= 1U;
  assert(ModbusRtu_ValidateReadResponse(valid, sizeof(valid), 1U, 1U, NULL) == MODBUS_RESULT_INVALID);
  assert(ModbusRtu_ValidateReadResponse(NULL, 0U, 1U, 1U, NULL) == MODBUS_RESULT_INVALID);
  assert(WaterSensor_DecodePh(valid + 3U, 2U, &ph));
  assert(fabsf(ph - 6.86F) < 0.001F);
  assert(WaterSensor_DecodeOxygen(oxygen_payload, sizeof(oxygen_payload), &oxygen, &temp));
  assert(fabsf(oxygen - 8.68F) < 0.001F && fabsf(temp - 22.52F) < 0.001F);
  assert(WaterSensor_DecodeSaturation(sat_payload, sizeof(sat_payload), &saturation));
  assert(fabsf(saturation - 96.5F) < 0.001F);
  oxygen_payload[7] = 5U;
  assert(!WaterSensor_DecodeOxygen(oxygen_payload, sizeof(oxygen_payload), &oxygen, &temp));
  assert(!WaterSensor_DecodePh(NULL, 2U, &ph));
  puts("PASS: Modbus CRC/count/exception bounds and real sensor register decoding");
}

static void TestSerial(void)
{
  char response[128];
  uint8_t byte;
  uint8_t ph_frame[] = {1U, 3U, 2U, 2U, 0xAEU, 0x38U, 0x98U};
  uint8_t reply_frame[16];
  uint16_t length;
  float ph = 0.0F;
  uint32_t previous;
  unsigned int i;
  uint8_t request[] = {1U, 3U, 0U, 0U, 0U, 1U, 0x84U, 0x0AU};
  tick = 10000U;
  assert(UartChannel_Init() == HAL_OK);
  assert(AtCommand_ClassifyLine("BOOK") == HAL_BUSY);
  assert(AtCommand_ClassifyLine("+INFO: OK") == HAL_BUSY);
  assert(AtCommand_ClassifyLine("OK") == HAL_OK);
  assert(AtCommand_ClassifyLine("+CME ERROR: 10") == HAL_ERROR);
  SetReply((const uint8_t *)"OK\r\nCONNECT OK\r\n", 16U);
  assert(AtCommand_ExecuteUntil(&huart2, "AT+MIPSTART=\"server\",1883",
    "CONNECT OK", response, sizeof(response), 100U) == HAL_OK);
  SetReply((const uint8_t *)"OK\r\n", 4U);
  assert(AtCommand_ExecuteUntil(&huart2, "AT+MIPSTART=\"server\",1883",
    "CONNECT OK", response, sizeof(response), 10U) == HAL_TIMEOUT);
  SetReply((const uint8_t *)"OK\r\nCONNECT FAIL\r\n", 18U);
  assert(AtCommand_ExecuteUntil(&huart2, "AT+MIPSTART=\"server\",1883",
    "CONNECT OK", response, sizeof(response), 100U) == HAL_ERROR);
  SetReply((const uint8_t *)"AT\r\n+INFO: BOOK\r\nOK\r\n", sizeof("AT\r\n+INFO: BOOK\r\nOK\r\n") - 1U);
  assert(Esp12fWifi_SendCommand("AT", response, sizeof(response), 100U) == HAL_OK);
  assert(Esp12fWifi_IsReady());
  SetReply((const uint8_t *)"\r\nERROR\r\n", 9U);
  assert(Air780_4G_SendCommand("AT", response, sizeof(response), 100U) == HAL_ERROR);
  SetReply((const uint8_t *)"+INFO: OK\r\n", 11U);
  assert(Esp12fWifi_SendCommand("AT", response, sizeof(response), 10U) == HAL_TIMEOUT);
  assert(!Esp12fWifi_IsReady());
  previous = tx_count;
  assert(Esp12fWifi_SendCommand("AT", NULL, 0U, 10U) == HAL_ERROR);
  assert(Esp12fWifi_SendCommand("AT\r\n", response, sizeof(response), 10U) == HAL_ERROR);
  assert(tx_count == previous);
  SetReply((const uint8_t *)"OK\r\n", 4U);
  assert(Esp12fWifi_SendCommand("AT", response, 2U, 10U) == HAL_ERROR);
  tx_result = HAL_TIMEOUT;
  assert(Esp12fWifi_SendCommand("AT", response, sizeof(response), 10U) == HAL_TIMEOUT);
  for (i = 0U; i < 2049U; ++i) { InjectByte(&huart2, 'X'); }
  assert(UartChannel_HasError(&huart2));
  irq_mask = 1U; UartChannel_Clear(&huart2); assert(irq_mask == 1U); irq_mask = 0U;
  assert(!UartChannel_HasError(&huart2));
  assert(!UartChannel_ReadByte(&huart2, &byte));
  HAL_UART_ErrorCallback(&huart2); assert(UartChannel_HasError(&huart2));
  Rs485_Init();
  SetReply(ph_frame, sizeof(ph_frame));
  assert(Rs485_Transfer(request, sizeof(request), reply_frame, sizeof(reply_frame), &length, 100U) == HAL_OK);
  assert(length == sizeof(ph_frame) && memcmp(reply_frame, ph_frame, length) == 0);
  previous = tick;
  assert(Rs485_Transfer(request, sizeof(request), reply_frame, sizeof(reply_frame), &length, 100U) == HAL_OK);
  assert(tx_tick - previous >= BOARD_RS485_QUERY_GAP_MS);
  SensorPower_Set(SENSOR_CHANNEL_1, true);
  assert(WaterSensor_ReadPh(&ph) == HAL_OK && fabsf(ph - 6.86F) < 0.001F);
  SensorPower_AllOff();
  assert(WaterSensor_ReadPh(&ph) == HAL_ERROR);
  SetReply(NULL, 0U);
  assert(Rs485_Transfer(request, sizeof(request), reply_frame, sizeof(reply_frame), &length, 10U) == HAL_TIMEOUT);
  puts("PASS: AT terminal lines/timeouts, UART overflow, immediate replies and RS485 spacing");
}

static void TestNetwork(void)
{
  const char *topic = "fish/device/test/water/latest";
  const char *json = "{\"ph\":7.12,\"doMgL\":8.68}";
  unsigned int i;
  network_mock = 1U;
  wifi_up = 1U;
  tick = 200000U;
  ControlClock_Init();
  RemoteControl_Init();
  NetworkManager_Init();
  for (i = 0U; i < 10U; ++i) { NetworkManager_Task(); }
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_WIFI);
  assert(MqttTransport_Publish(MQTT_LINK_WIFI, topic, (const uint8_t *)json,
    (uint16_t)strlen(json)) == MQTT_RESULT_OK);
  assert(raw_used == strlen(json) && memcmp(raw_copy, json, raw_used) == 0);
  assert(MqttTransport_Publish(MQTT_LINK_4G, topic, (const uint8_t *)json,
    (uint16_t)strlen(json)) == MQTT_RESULT_NOT_CONNECTED);
  {
    uint8_t oversized[512] = {0};
    assert(MqttTransport_Publish(MQTT_LINK_WIFI, topic, oversized, sizeof(oversized))
      == MQTT_RESULT_INVALID_ARGUMENT);
  }
  /* 上电稳定后四项全部读取成功才允许一次发布；失败不能沿用旧读数。 */
  sensor_mock = 1U;
  WaterReporter_Init();
  assert(SensorPower_IsOn(SENSOR_CHANNEL_1) && SensorPower_IsOn(SENSOR_CHANNEL_2));
  tick += 10000U;
  i = wifi_publishes;
  WaterReporter_Task();
  assert(wifi_publishes == i + 1U);
  assert(strstr((char *)raw_copy, "\"ph\":7.12") != NULL);
  assert(strstr((char *)raw_copy, "\"doSaturation\":96.50") != NULL);
  sensor_mock = 0U;
  SetReply(NULL, 0U);
  tick += 3600000U;
  WaterReporter_Task();
  assert(wifi_publishes == i + 1U);
  SensorPower_AllOff();
  WaterReporter_Task();
  sensor_mock = 1U;
  tick += 30000U;
  SensorPower_AllOn();
  i = sensor_queries;
  WaterReporter_Task();
  assert(sensor_queries == i);
  tick += 10000U;
  WaterReporter_Task();
  assert(sensor_queries == i + 3U);
  sensor_mock = 0U;
  SensorPower_AllOff();
  assert(wifi_id_seen != 0U);

  /* 现场仅 Wi-Fi，重试时即使 4G RDY 拉高也不得切换蜂窝链路。 */
  wifi_up = 0U;
  GPIOD->pins |= GPIO_PIN_4;
  tick += 40000U;
  NetworkManager_Task();
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_NONE);
  for (i = 0U; i < 28U; ++i)
  {
    NetworkManager_Task();
    tick += 16000U;
  }
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_NONE);
  assert(cell_id_seen == 0U);
  assert(MqttTransport_Publish(MQTT_LINK_4G, topic, (const uint8_t *)json,
    (uint16_t)strlen(json)) == MQTT_RESULT_NOT_CONNECTED);
  assert(cell_publishes == 0U);
  /* Wi-Fi 恢复后继续上报，4G 保留接口但禁用。 */
  wifi_up = 1U;
  tick += 61000U;
  for (i = 0U; i < 12U; ++i) { NetworkManager_Task(); }
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_WIFI);
  assert(MqttTransport_Publish(MQTT_LINK_4G, topic, (const uint8_t *)json,
    (uint16_t)strlen(json)) == MQTT_RESULT_NOT_CONNECTED);

  /* Wi-Fi 不可用时始终保持离线，不运行保留的 4G 重试接口。 */
  wifi_up = 0U;
  cell_fail_connect_once = 1U;
  cell_socket_open = 0U;
  tick += 61000U;
  NetworkManager_Init();
  for (i = 0U; i < 35U; ++i)
  {
    NetworkManager_Task();
    tick += 16000U;
  }
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_NONE);
  /* Wi-Fi 再恢复可重新认证并订阅。 */
  cell_up = 0U;
  wifi_up = 0U;
  tick += 61000U;
  NetworkManager_Init();
  for (i = 0U; i < 9U; ++i)
  {
    NetworkManager_Task();
    tick += 16000U;
  }
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_NONE);
  wifi_up = 1U;
  for (i = 0U; i < 22U; ++i)
  {
    NetworkManager_Task();
    tick += 16000U;
  }
  assert(NetworkManager_GetActiveLink() == MQTT_LINK_WIFI);
  network_mock = 0U;
  puts("PASS: Wi-Fi RAW publish/subscription/recovery, 4G disabled and one active link");
}

int main(void)
{
  TestOutputs(); TestMotion(); TestProtocol(); TestSerial(); TestNetwork();
  puts("ALL BASELINE TESTS PASSED");
  return 0;
}
