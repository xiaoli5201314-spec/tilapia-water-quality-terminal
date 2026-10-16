/* 文件：test_network_recovery.c */
/* 功能：复现慢速发布应答和旧 MQTT 会话引起的重连失败，不连接实物。 */
#define main baseline_main
#include "test_baseline.c"
#undef main

/* 模块发布成功应答晚于一秒到达；使用真实解析、事务和链路管理代码。 */
static void DeliverDelayedAck(void)
{
  const char *line="+MQTTPUB:OK\r\n";
  unsigned int i;
  if (raw_prompt_pending && (int32_t)(tick-raw_prompt_at)>=0)
  { raw_prompt_pending=0U; InjectByte(&huart1,'>'); }
  if (!raw_ack_pending || (int32_t)(tick-raw_ack_at)<0) { return; }
  raw_ack_pending=0U;
  for (i=0U;line[i]!='\0';++i) { InjectByte(&huart1,(uint8_t)line[i]); }
}

/* 模拟模块独立供电，STM32 的软件初始化不会清除模块已有 MQTT 会话。 */
static void Connect(void)
{
  unsigned int i;
  tick=1000U; network_mock=1U; wifi_up=1U;
  raw_success=1U; raw_ack_delay=0U; raw_ack_pending=0U;
  raw_prompt_delay=0U; raw_prompt_pending=0U; raw_tx_cost=0U;
  network_session_mock=1U; wifi_mqtt_connected=0U;
  assert(UartChannel_Init()==HAL_OK);
  Actuator_Init(); BrushMotor_Init(); CleaningController_Init();
  ControlClock_Init(); ControlConfig_ResetStorage(); RemoteControl_Init();
  AtCommand_Init(); NetworkManager_Init();
  for (i=0U;i<12U;++i) { NetworkManager_Task(); }
  assert(NetworkManager_GetActiveLink()==MQTT_LINK_WIFI);
}

/* 1500 ms 后的成功应答不能被误当断网，同时必须维护毛刷停止期限。 */
static void TestDelayedAck(void)
{
  const char *json="{\"ph\":7.12,\"doMgL\":8.68}";
  Connect();
  assert(BrushMotor_Start(200U)==HAL_OK);
  raw_ack_delay=1500U; delay_hook=DeliverDelayedAck;
  assert(MqttTransport_PublishRaw(MQTT_LINK_WIFI,RemoteControl_StatusTopic(),
    (const uint8_t *)json,(uint16_t)strlen(json))==MQTT_RESULT_OK);
  assert(NetworkManager_GetActiveLink()==MQTT_LINK_WIFI);
  assert(!BrushMotor_IsRunning() && !raw_ack_pending);
  assert(g_at_raw_trace.stage==AT_RAW_DONE && g_at_raw_trace.result==HAL_OK);
  assert(g_at_raw_trace.sent_bytes==strlen(json));
  delay_hook=NULL;
  puts("PASS: delayed RAW acknowledgement remains connected and motor deadline is maintained");
}

/* 发布应答缺失，但模块 MQTT 仍连接时，重试必须真正恢复发布与订阅。 */
static void TestExistingSessionRecovery(void)
{
  const char *json="{\"ph\":7.12}";
  unsigned int i;
  Connect();
  raw_success=0U;
  assert(MqttTransport_PublishRaw(MQTT_LINK_WIFI,RemoteControl_StatusTopic(),
    (const uint8_t *)json,(uint16_t)strlen(json))==MQTT_RESULT_NOT_CONNECTED);
  assert(NetworkManager_GetActiveLink()==MQTT_LINK_NONE && wifi_mqtt_connected);
  assert(g_at_raw_trace.stage==AT_RAW_ACK && g_at_raw_trace.result==HAL_TIMEOUT);
  raw_success=1U; tick+=5001U;
  for (i=0U;i<12U;++i) { NetworkManager_Task(); }
  assert(NetworkManager_GetActiveLink()==MQTT_LINK_WIFI);
  assert(MqttTransport_PublishRaw(MQTT_LINK_WIFI,RemoteControl_StatusTopic(),
    (const uint8_t *)json,(uint16_t)strlen(json))==MQTT_RESULT_OK);
  puts("PASS: an existing module MQTT session is cleaned before reconfiguration");
}

/* 空会话清理返回 ERROR 不能阻止首次配置；连接成功仍须查询与订阅确认。 */
static void TestEmptySessionCleanup(void)
{
  const char *json="{\"ph\":7.12}";
  wifi_clean_empty_error=1U;
  Connect();
  assert(MqttTransport_PublishRaw(MQTT_LINK_WIFI,RemoteControl_StatusTopic(),
    (const uint8_t *)json,(uint16_t)strlen(json))==MQTT_RESULT_OK);
  wifi_clean_empty_error=0U;
  puts("PASS: empty-session cleanup does not prevent first connection");
}

/* 提示符迟到不能导致只发半个正文，也不能挤掉随后发布应答的等待时间。 */
static void TestSeparateRawBudgets(void)
{
  uint8_t payload[300];
  memset(payload,'x',sizeof(payload));
  Connect();
  raw_prompt_delay=995U; raw_ack_delay=995U; raw_tx_cost=3U;
  delay_hook=DeliverDelayedAck;
  assert(AtCommand_PublishRaw(&huart1,
    "AT+MQTTPUBRAW=0,\"test\",300,0,1",payload,sizeof(payload),1000U)==HAL_OK);
  assert(raw_used==sizeof(payload) && memcmp(raw_copy,payload,sizeof(payload))==0);
  assert(g_at_raw_trace.sent_bytes==300U && !raw_ack_pending);
  delay_hook=NULL;
  puts("PASS: prompt, full payload and final acknowledgement each have their own time budget");
}

int main(int argc, char **argv)
{
  if (argc==2 && strcmp(argv[1],"recovery")==0) { TestExistingSessionRecovery(); }
  else if (argc==2 && strcmp(argv[1],"empty")==0) { TestEmptySessionCleanup(); }
  else if (argc==2 && strcmp(argv[1],"budgets")==0) { TestSeparateRawBudgets(); }
  else { TestDelayedAck(); }
  return 0;
}
