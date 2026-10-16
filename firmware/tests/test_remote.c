/* 文件：test_remote.c */
/* 功能：在假串口和 GPIO 上验证真实控制协议、时钟、调度、RAW 与配置，不访问实机。 */
#define main baseline_main
#include "test_baseline.c"
#undef main
#include "control_protocol.h"
#include "control_clock.h"
#include "control_config.h"
#include "remote_control.h"
#include "at_stream.h"

/* 用固定 UTC 时间独立构造命令，避免预期由被测实现计算。 */
static void Command(const char *id, const char *fields)
{
  char text[512];
  int n = snprintf(text, sizeof(text),
    "{\"v\":1,\"id\":\"%s\",\"issued\":1791770000,\"ttl\":30,%s}", id, fields);
  assert(n > 0 && (size_t)n < sizeof(text));
  RemoteControl_Receive(RemoteControl_CommandTopic(), (const uint8_t *)text,
                        (uint16_t)n);
  RemoteControl_Poll();
  RemoteControl_PersistTask();
}

/* 每例仅重置假硬件与运行状态，真实配置编码和控制实现参与测试。 */
static void ResetControl(bool sync)
{
  tick = 1000U;
  network_mock = 0U;
  SensorPower_AllOn();
  Actuator_Init();
  BrushMotor_Init();
  CleaningController_Init();
  ControlClock_Init();
  ControlConfig_ResetStorage();
  RemoteControl_Init();
  if (sync) { ControlClock_SetUnix(1791770000U); }
  RemoteControl_SetConnected(true);
  tick += 100U;
}

/* 拒绝错误类型、重复键、伪 JSON 和未知字段，且不接受超出 30 秒的机械动作。 */
static void TestSchema(void)
{
  ControlCommand c;
  char id[13];
  const char *valid = "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"brush\","
    "\"issued\":1791770000,\"ttl\":30,\"action\":\"start\",\"duration\":30}";
  const char *bad[] = {
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":1,\"ttl\":30,\"extra\":0}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":1,\"ttl\":30,\"v\":1}",
    "{\"v\":1 \"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":1,\"ttl\":30}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":1,\"ttl\":30,}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":\"1\",\"ttl\":30}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":01,\"ttl\":30}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":1.0,\"ttl\":30}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"stop\",\"issued\":1,\"ttl\":30}{}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"brush\",\"issued\":1,\"ttl\":30,\"action\":\"start\",\"duration\":31}",
    "{\"v\":1,\"id\":\"0123456789AB\",\"op\":\"stop\",\"issued\":1,\"ttl\":30}",
    "{\"v\":1,\"id\":\"0123456789ab\",\"op\":\"cleaning_config\",\"issued\":1,\"ttl\":30,"
      "\"enabled\":1,\"interval\":60,\"extend\":1,\"brush\":1,\"retract\":1,\"settle\":0}"
  };
  size_t i;
  assert(ControlProtocol_Parse((const uint8_t *)valid, (uint16_t)strlen(valid), &c, id));
  assert(c.duration == 30U && strcmp(c.id, "0123456789ab") == 0);
  for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
  { assert(!ControlProtocol_Parse((const uint8_t *)bad[i], (uint16_t)strlen(bad[i]), &c, id)); }
  puts("PASS: strict flat schema, types, duplicate/unknown keys, bounds and JSON grammar");
}

/* 北京时间样例需换算 UTC，非法日历与星期错误不可标记同步。 */
static void TestClock(void)
{
  uint32_t value;
  assert(ControlClock_Parse("Mon Oct 12 12:33:20 2026", &value));
  assert(value == 1791779600U);
  assert(ControlClock_Parse("Thu Feb 29 08:00:00 2024", &value));
  assert(value == 1709164800U);
  assert(!ControlClock_Parse("Thu Feb 29 08:00:00 2025", &value));
  assert(!ControlClock_Parse("Tue Oct 12 12:33:20 2026", &value));
  assert(!ControlClock_Parse("Mon Oct 12 24:00:00 2026", &value));
  tick = UINT32_MAX - 499U;
  ControlClock_Init();
  ControlClock_SetUnix(1791770000U);
  tick += 1500U;
  assert(ControlClock_Unix() == 1791770001U);
  tick += 86400001U;
  assert(!ControlClock_IsSynced());
  puts("PASS: SNTP English calendar, Beijing UTC, leap date, weekday, rollover and expiry");
}

/* 未校时只允许查询/停止；重复、过期和未来命令不能刷新或启动动作。 */
static void TestAdmission(void)
{
  ResetControl(false);
  Command("000000000001", "\"op\":\"mode\",\"mode\":\"manual\"");
  assert(RemoteControl_AckRejected());
  Command("000000000002", "\"op\":\"stop\"");
  assert(!RemoteControl_AckRejected());
  assert(RemoteControl_Config()->manual == 1U);
  Command("000000000003", "\"op\":\"brush\",\"action\":\"start\",\"duration\":1");
  assert(!BrushMotor_IsRunning() && RemoteControl_AckRejected());
  ControlClock_SetUnix(1791770000U);
  Command("000000000004", "\"op\":\"brush\",\"action\":\"start\",\"duration\":1");
  assert(BrushMotor_IsRunning());
  tick += 500U;
  Command("000000000004", "\"op\":\"brush\",\"action\":\"start\",\"duration\":1");
  assert(RemoteControl_AckRejected());
  tick += 501U; RemoteControl_Poll();
  assert(!BrushMotor_IsRunning());
  tick += 30000U;
  Command("000000000005", "\"op\":\"brush\",\"action\":\"start\",\"duration\":1");
  assert(!BrushMotor_IsRunning() && RemoteControl_AckRejected());
  ControlClock_SetUnix(1791769994U);
  Command("000000000006", "\"op\":\"brush\",\"action\":\"start\",\"duration\":1");
  assert(!BrushMotor_IsRunning() && RemoteControl_AckRejected());
  puts("PASS: unsynced admission, stop, TTL, future skew and no duplicate extension");
}

/* 手动禁止自动动作，完整清洗占用传感器但不改变传感器供电。 */
static void TestArbitration(void)
{
  ResetControl(true);
  Command("000000000010", "\"op\":\"mode\",\"mode\":\"manual\"");
  tick += 61000U; RemoteControl_Poll();
  assert(!Actuator_IsBusy() && !BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770000U);
  Command("000000000011", "\"op\":\"actuator\",\"action\":\"extend\",\"duration\":30");
  assert(Actuator_IsBusy());
  assert(!RemoteControl_CollectionActive());
  Command("000000000012", "\"op\":\"actuator\",\"action\":\"retract\",\"duration\":1");
  assert(Actuator_GetState() == ACTUATOR_STOP && Actuator_IsBusy());
  tick += 99U; RemoteControl_Poll(); assert(Actuator_GetState() == ACTUATOR_STOP);
  tick += 1U; RemoteControl_Poll(); assert(Actuator_GetState() == ACTUATOR_REVERSE);
  Command("000000000013", "\"op\":\"mode\",\"mode\":\"auto\"");
  assert(!Actuator_IsBusy());
  Command("000000000014", "\"op\":\"cleaning_config\",\"enabled\":false,"
    "\"interval\":60,\"extend\":1,\"brush\":1,\"retract\":1,\"settle\":0");
  Command("000000000015", "\"op\":\"cleaning\",\"action\":\"start\"");
  assert(CleaningController_IsBusy());
  Command("000000000016", "\"op\":\"cleaning\",\"action\":\"start\"");
  assert(RemoteControl_AckRejected());
  tick += 100U; RemoteControl_Poll(); /* 推杆此前刚停止，先度过换向保护。 */
  tick += 1000U; RemoteControl_Poll();
  assert(CleaningController_GetState() == CLEANING_BRUSHING);
  tick += 1000U; RemoteControl_Poll();
  assert(CleaningController_GetState() == CLEANING_RETRACTING);
  tick += 1000U; RemoteControl_Poll();
  RemoteControl_Poll();
  assert(!CleaningController_IsBusy());
  assert(SensorPower_IsOn(SENSOR_CHANNEL_1) && SensorPower_IsOn(SENSOR_CHANNEL_2));
  Command("000000000017", "\"op\":\"cleaning\",\"action\":\"start\"");
  Command("000000000018", "\"op\":\"cleaning\",\"action\":\"stop\"");
  assert(!Actuator_IsBusy() && !BrushMotor_IsRunning());
  assert(RemoteControl_Config()->manual == 1U);
  puts("PASS: manual/auto arbitration, guarded reversal, full cleaning, stop and powered sensors");
}

/* 采集跨午夜边界与独立周期，以字面北京时间独立验证。 */
static void TestSampling(void)
{
  ResetControl(true);
  Command("000000000020", "\"op\":\"sampling\",\"mode\":\"schedule\","
    "\"start\":1380,\"end\":60,\"sample\":2,\"publish\":5");
  ControlClock_SetUnix(1791822000U); /* 2026-10-13 00:20 北京时间。 */
  assert(RemoteControl_CollectionActive());
  ControlClock_SetUnix(1791825600U); /* 01:20 已超过结束分钟。 */
  assert(!RemoteControl_CollectionActive());
  assert(RemoteControl_Config()->sample == 2U && RemoteControl_Config()->publish == 5U);
  ControlClock_SetUnix(1791770000U);
  Command("000000000021", "\"op\":\"sampling\",\"mode\":\"schedule\","
    "\"start\":60,\"end\":60,\"sample\":2,\"publish\":5");
  assert(RemoteControl_AckRejected());
  puts("PASS: cross-midnight schedule, independent periods and invalid equal endpoints");
}

/* 持久化签名、CRC 和配置范围全部生效；手动动作从不恢复。 */
static void TestPersistence(void)
{
  ControlConfig original, loaded;
  uint8_t record[CONTROL_CONFIG_RECORD_SIZE];
  ResetControl(true);
  Command("000000000030", "\"op\":\"mode\",\"mode\":\"manual\"");
  RemoteControl_PersistTask();
  original = *RemoteControl_Config();
  assert(ControlConfig_Encode(&original, 1U, record));
  assert(ControlConfig_Decode(record, &loaded, NULL));
  assert(loaded.manual == 1U);
  record[20] ^= 1U;
  assert(!ControlConfig_Decode(record, &loaded, NULL));
  original.brush_plan.duration = 31U;
  assert(!ControlConfig_Encode(&original, 2U, record));
  Command("000000000031", "\"op\":\"brush\",\"action\":\"start\",\"duration\":30");
  RemoteControl_Init();
  assert(RemoteControl_Config()->manual == 1U);
  assert(!BrushMotor_IsRunning() && !Actuator_IsBusy());
  puts("PASS: config CRC, version/ranges, saved manual mode and reboot safe outputs");
}

/* 丢失初始保护等待或 AT 阻塞停止处理均会使本例失败。 */
static unsigned int stop_injected;
static void InjectStopDuringWait(void)
{
  char frame[512];
  const char *text="{\"v\":1,\"id\":\"000000000042\",\"op\":\"stop\","
    "\"issued\":1791770000,\"ttl\":30}";
  unsigned int i;
  int n;
  if (stop_injected || tick<1250U) { return; }
  stop_injected=1U;
  n=snprintf(frame,sizeof(frame),"+MQTTSUBRECV:0,\"%s\",%u,%s\r\n",
    RemoteControl_CommandTopic(),(unsigned)strlen(text),text);
  assert(n>0 && (size_t)n<sizeof(frame));
  for (i=0U;i<(unsigned int)n;++i) { InjectByte(&huart1,(uint8_t)frame[i]); }
}
/* 等待没有 AT 应答时收到停止 URC，50毫秒内关闭实际 GPIO，且无嵌套发送。 */
static void TestStopWait(void)
{
  char response[32];
  unsigned int sent;
  ResetControl(true); AtCommand_Init();
  Command("000000000040","\"op\":\"mode\",\"mode\":\"manual\"");
  Command("000000000041","\"op\":\"brush\",\"action\":\"start\",\"duration\":30");
  assert(BrushMotor_IsRunning());
  SetReply(NULL,0U); sent=tx_count; stop_injected=0U; delay_hook=InjectStopDuringWait;
  assert(AtCommand_Execute(&huart1,"AT",response,sizeof(response),300U)==HAL_TIMEOUT);
  delay_hook=NULL;
  assert(stop_injected && !BrushMotor_IsRunning() && tx_count==sent+1U);
  assert(RemoteControl_Config()->manual==1U);
  Command("000000000043","\"op\":\"brush\",\"action\":\"start\",\"duration\":1");
  assert(BrushMotor_IsRunning());
  assert(AtCommand_Execute(&huart1,"AT",response,sizeof(response),1100U)==HAL_TIMEOUT);
  assert(!BrushMotor_IsRunning());
  puts("PASS: stop URC during AT wait, no reentrant AT and motor deadline while waiting");
}

/* 前半 URC 在事务前到达，后半混合正文内 OK；不得清缓存或误判应答。 */
static void TestFragments(void)
{
  char frame[512], response[64];
  const char *text="{\"v\":1,\"id\":\"000000000050\",\"op\":\"stop\",\"issued\":1791770000,\"ttl\":30}";
  int n, i, split;
  ResetControl(true); AtCommand_Init();
  Command("000000000049","\"op\":\"mode\",\"mode\":\"manual\"");
  Command("000000000048","\"op\":\"brush\",\"action\":\"start\",\"duration\":30");
  n=snprintf(frame,sizeof(frame),"+MQTTSUBRECV:0,\"%s\",%u,%s\r\nOK\r\n",
    RemoteControl_CommandTopic(),(unsigned)strlen(text),text);
  assert(n>0 && (size_t)n<sizeof(frame));
  split=n/2;
  for (i=0;i<split;++i) { InjectByte(&huart1,(uint8_t)frame[i]); }
  AtCommand_Poll();
  assert(BrushMotor_IsRunning());
  SetReply((const uint8_t *)frame+split,(uint16_t)(n-split));
  assert(AtCommand_Execute(&huart1,"AT",response,sizeof(response),100U)==HAL_OK);
  assert(!BrushMotor_IsRunning() && strstr(response,"MQTTSUBRECV")==NULL);
  assert(strcmp(response,"OK\r\n")==0);
  puts("PASS: fragmented URC spans AT boundary, preserved buffer and separated terminal OK");
}

/* 真实 RAW 只发声明长度，无转义和 CRLF；初始 OK 不能确认成功。 */
static void TestRaw(void)
{
  const char *text="{\"ph\":7.12,\"doMgL\":8.68}";
  unsigned int i;
  ResetControl(true); AtCommand_Init(); network_mock=1U; wifi_up=1U;
  NetworkManager_Init();
  for (i=0U;i<12U;++i) { NetworkManager_Task(); }
  assert(NetworkManager_GetActiveLink()==MQTT_LINK_WIFI);
  raw_success=1U;
  assert(MqttTransport_PublishRaw(MQTT_LINK_WIFI,RemoteControl_StatusTopic(),
    (const uint8_t *)text,(uint16_t)strlen(text))==MQTT_RESULT_OK);
  assert(raw_used==strlen(text) && memcmp(raw_copy,text,raw_used)==0);
  raw_success=0U;
  assert(MqttTransport_PublishRaw(MQTT_LINK_WIFI,RemoteControl_StatusTopic(),
    (const uint8_t *)text,(uint16_t)strlen(text))==MQTT_RESULT_NOT_CONNECTED);
  assert(NetworkManager_GetActiveLink()==MQTT_LINK_NONE);
  raw_success=1U; network_mock=0U;
  puts("PASS: exact RAW bytes, prompt, no escaping, final +MQTTPUB:OK required");
}

/* 四片在机械运行中发布且内容冻结，五秒重复发布，变化立即生成新 seq。 */
static void TestStatusPublishing(void)
{
  char frozen[CONTROL_STATUS_CAPACITY];
  unsigned int i, count;
  ResetControl(true); AtCommand_Init(); network_mock=1U; wifi_up=1U;
  NetworkManager_Init();
  for (i=0U;i<12U;++i) { NetworkManager_Task(); }
  Command("000000000090","\"op\":\"mode\",\"mode\":\"manual\"");
  Command("000000000091","\"op\":\"brush\",\"action\":\"start\",\"duration\":30");
  assert(BrushMotor_IsRunning());
  count=wifi_publishes;
  RemoteControl_StatusTask();
  assert(wifi_publishes==count+4U && BrushMotor_IsRunning());
  RemoteControl_StatusTask();
  assert(wifi_publishes==count+4U);
  tick+=5000U; RemoteControl_Poll(); RemoteControl_StatusTask();
  assert(wifi_publishes==count+8U);
  memcpy(frozen,RemoteControl_StatusPart(1),sizeof(frozen));
  Command("000000000092","\"op\":\"brush\",\"action\":\"stop\"");
  assert(!BrushMotor_IsRunning());
  assert(memcmp(frozen,RemoteControl_StatusPart(1),sizeof(frozen))==0);
  RemoteControl_StatusTask();
  assert(wifi_publishes==count+12U);
  assert(strstr(RemoteControl_StatusPart(1),"000000000092")!=NULL);
  network_mock=0U;
  puts("PASS: four once-only frozen parts, heartbeat during motion and change sequence");
}

/* 真实 AT 校时路径必须启用北京时间时区并完整解析 URC 前缀后日历。 */
static void TestSntpIntegration(void)
{
  unsigned int i;
  ResetControl(false); AtCommand_Init(); network_mock=1U; wifi_up=1U;
  NetworkManager_Init();
  for (i=0U;i<12U;++i) { NetworkManager_Task(); }
  ControlClock_Task();
  assert(strstr((const char *)request_copy,"AT+CIPSNTPCFG=1,8,")!=NULL);
  assert(!ControlClock_IsSynced());
  tick+=10000U; ControlClock_Task();
  assert(ControlClock_IsSynced() && ControlClock_Unix()==1791779600U);
  network_mock=0U;
  puts("PASS: actual SNTP config/query path, zone8 and CIPSNTPTIME prefix parsing");
}

/* 所有合法配置最大值、最大时间/序号，经真实序列化器导出父任务集成夹具。 */
static void TestStatusPackets(void)
{
  FILE *output;
  unsigned int i;
  ResetControl(true);
  Command("000000000060","\"op\":\"sampling\",\"mode\":\"schedule\",\"start\":1439,"
    "\"end\":1438,\"sample\":3600,\"publish\":86400");
  Command("000000000061","\"op\":\"plan\",\"target\":\"brush\",\"type\":\"interval\","
    "\"start\":1439,\"interval\":86400,\"duration\":30,\"action\":\"start\"");
  Command("000000000062","\"op\":\"plan\",\"target\":\"actuator\",\"type\":\"interval\","
    "\"start\":1439,\"interval\":86400,\"duration\":30,\"action\":\"retract\"");
  Command("000000000063","\"op\":\"cleaning_config\",\"enabled\":true,\"interval\":604800,"
    "\"extend\":30,\"brush\":30,\"retract\":30,\"settle\":600");
  Command("000000000064","\"op\":\"mode\",\"mode\":\"manual\"");
  tick=UINT32_MAX;
  ControlClock_SetUnix(UINT32_MAX);
  assert(RemoteControl_CaptureStatus(UINT32_MAX));
  output=fopen("tests/status_packets.jsonl","w"); assert(output!=NULL);
  for (i=0U;i<4U;++i)
  {
    const char *part=RemoteControl_StatusPart(i);
    size_t length=strlen(part);
    assert(length<400U && length+strlen(RemoteControl_StatusTopic())+8U<=512U);
    assert(strstr(part,"\"seq\":4294967295")!=NULL);
    assert(fprintf(output,"%s\n",part)>0);
    printf("PASS: status part %u = %zu bytes, MQTT budget = %zu\n",i,length,
      length+strlen(RemoteControl_StatusTopic())+8U);
  }
  assert(fclose(output)==0);
  assert(strstr(RemoteControl_StatusPart(0),"\"uptimeMs\":4294967295")!=NULL);
  assert(strstr(RemoteControl_StatusPart(3),"\"interval\":604800")!=NULL);
  /* 不同步的拒绝回执之后恢复时钟，最大剩余/最长方向与灯色均来自真实模块。 */
  ControlClock_Init();
  Command("000000000065","\"op\":\"brush\",\"action\":\"start\",\"duration\":30");
  assert(RemoteControl_AckRejected());
  tick=UINT32_MAX-101U; Actuator_Init();
  tick=UINT32_MAX;
  ControlClock_SetUnix(UINT32_MAX);
  assert(BrushMotor_Start(30000U)==HAL_OK);
  assert(Actuator_Move(ACTUATOR_REVERSE,30000U)==HAL_OK);
  AlarmLight_Set(ALARM_LIGHT_YELLOW);
  assert(RemoteControl_CaptureStatus(UINT32_MAX));
  output=fopen("tests/status_packets.jsonl","w"); assert(output!=NULL);
  for (i=0U;i<4U;++i)
  {
    const char *part=RemoteControl_StatusPart(i);
    size_t length=strlen(part);
    assert(length<400U && length+strlen(RemoteControl_StatusTopic())+8U<=512U);
    assert(fprintf(output,"%s\n",part)>0);
    printf("PASS: worst running/rejection part %u = %zu bytes, MQTT budget = %zu\n",
      i,length,length+strlen(RemoteControl_StatusTopic())+8U);
  }
  assert(fclose(output)==0);
  assert(strstr(RemoteControl_StatusPart(1),"\"remaining\":30")!=NULL);
  assert(strstr(RemoteControl_StatusPart(1),"\"state\":\"retracting\"")!=NULL);
  assert(strstr(RemoteControl_StatusPart(1),"\"message\":\"clock_not_ready\"")!=NULL);
  CleaningController_Cancel();
}

/* 默认/保存周期不得被上电 60 秒例外覆盖，手动与时钟跳变不补跑计划。 */
static void TestPlans(void)
{
  ResetControl(true);
  RemoteControl_Poll();
  tick+=61000U; RemoteControl_Poll();
  assert(!Actuator_IsBusy() && !BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770000U);
  Command("000000000070","\"op\":\"cleaning_config\",\"enabled\":false,\"interval\":60,"
    "\"extend\":1,\"brush\":1,\"retract\":1,\"settle\":0");
  Command("000000000071","\"op\":\"plan\",\"target\":\"brush\",\"type\":\"interval\","
    "\"start\":600,\"interval\":60,\"duration\":2,\"action\":\"start\"");
  tick+=59999U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  tick+=1U; RemoteControl_Poll(); assert(BrushMotor_IsRunning());
  tick+=2000U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770000U);
  Command("000000000072","\"op\":\"mode\",\"mode\":\"manual\"");
  tick+=120000U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770000U);
  Command("000000000073","\"op\":\"mode\",\"mode\":\"auto\"");
  assert(!BrushMotor_IsRunning());
  tick+=59999U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  ControlClock_SetUnix(1791775000U); RemoteControl_Poll();
  tick+=1U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770000U);
  Command("000000000074","\"op\":\"plan\",\"target\":\"brush\",\"type\":\"daily\","
    "\"start\":600,\"interval\":60,\"duration\":1,\"action\":\"start\"");
  ControlClock_SetUnix(1791770399U); RemoteControl_Poll(); /* 北京09:59:59 */
  tick+=1000U; RemoteControl_Poll(); assert(BrushMotor_IsRunning());
  tick+=1000U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770399U); RemoteControl_Poll();
  tick+=1000U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  ControlClock_SetUnix(1791770000U);
  Command("000000000075","\"op\":\"plan\",\"target\":\"brush\",\"type\":\"off\","
    "\"start\":600,\"interval\":60,\"duration\":1,\"action\":\"start\"");
  tick+=61000U; RemoteControl_Poll(); assert(!BrushMotor_IsRunning());
  puts("PASS: persisted cleaning interval, interval/daily/off, manual and clock jump no replay");
}

/* 日志满模拟真实存储失败：未保存前不可成功回执，失败回滚普通设置。 */
static void TestStorageFailure(void)
{
  ControlConfig c;
  uint32_t i;
  const char *text="{\"v\":1,\"id\":\"000000000080\",\"op\":\"sampling\","
    "\"issued\":1791770000,\"ttl\":30,\"mode\":\"pause\",\"start\":480,\"end\":1080,"
    "\"sample\":2,\"publish\":5}";
  ResetControl(true);
  c=*RemoteControl_Config();
  for (i=0U;i<CONTROL_CONFIG_FLASH_SIZE/CONTROL_CONFIG_RECORD_SIZE;++i)
  { c.sample=2U+i%100U; assert(ControlConfig_Save(&c)); }
  RemoteControl_Receive(RemoteControl_CommandTopic(),(const uint8_t *)text,(uint16_t)strlen(text));
  RemoteControl_Poll();
  assert(RemoteControl_CaptureStatus(1U));
  assert(strstr(RemoteControl_StatusPart(1),"000000000080")==NULL);
  RemoteControl_PersistTask();
  assert(RemoteControl_AckRejected());
  assert(RemoteControl_Config()->collection_mode==COLLECTION_RUN);
  assert(RemoteControl_CaptureStatus(2U));
  assert(strstr(RemoteControl_StatusPart(1),"\"message\":\"storage_error\"")!=NULL);
  puts("PASS: full log failure, no premature success ACK, config rollback and storage_error");
}

/* 一小时采样周期恢复后必须重新读三组寄存器，失败时不得发布暂停前缓存。 */
static void TestResumeCache(void)
{
  unsigned int i, queries, publishes;
  ResetControl(true); AtCommand_Init(); WaterSensor_Init();
  network_mock=1U; wifi_up=1U; sensor_mock=1U;
  NetworkManager_Init();
  for (i=0U;i<12U;++i) { NetworkManager_Task(); }
  Command("0000000000a0","\"op\":\"sampling\",\"mode\":\"run\",\"start\":480,"
    "\"end\":1080,\"sample\":2,\"publish\":5");
  WaterReporter_Init();
  tick+=10000U; WaterReporter_Task();
  queries=sensor_queries; publishes=wifi_publishes;
  Command("0000000000a1","\"op\":\"sampling\",\"mode\":\"pause\",\"start\":480,"
    "\"end\":1080,\"sample\":3600,\"publish\":5");
  WaterReporter_Task();
  tick+=6000U;
  Command("0000000000a2","\"op\":\"sampling\",\"mode\":\"run\",\"start\":480,"
    "\"end\":1080,\"sample\":3600,\"publish\":5");
  WaterReporter_Task();
  assert(sensor_queries==queries+3U && wifi_publishes==publishes+1U);
  queries=sensor_queries; publishes=wifi_publishes;
  Command("0000000000a3","\"op\":\"cleaning_config\",\"enabled\":false,\"interval\":60,"
    "\"extend\":1,\"brush\":1,\"retract\":1,\"settle\":0");
  Command("0000000000a4","\"op\":\"cleaning\",\"action\":\"start\"");
  /* AT 等待会调用 RemoteControl_Poll，完整清洗期间没有重新进入采集 Task。 */
  tick+=1000U; RemoteControl_Poll();
  tick+=1000U; RemoteControl_Poll();
  tick+=1000U; RemoteControl_Poll(); RemoteControl_Poll();
  assert(!CleaningController_IsBusy());
  tick+=2000U; WaterReporter_Task();
  assert(sensor_queries==queries+3U && wifi_publishes==publishes+1U);
  publishes=wifi_publishes;
  ControlClock_SetUnix(1791770000U);
  Command("0000000000a5","\"op\":\"sampling\",\"mode\":\"pause\",\"start\":480,"
    "\"end\":1080,\"sample\":3600,\"publish\":5");
  WaterReporter_Task(); tick+=6000U;
  sensor_mock=0U; SetReply(NULL,0U);
  Command("0000000000a6","\"op\":\"sampling\",\"mode\":\"run\",\"start\":480,"
    "\"end\":1080,\"sample\":3600,\"publish\":5");
  WaterReporter_Task();
  assert(wifi_publishes==publishes);
  network_mock=0U;
  puts("PASS: sample3600 pause/cleaning resume requires fresh reads; failure never publishes old cache");
}

/* 主机仅启动本地真实代码，未连接网络/硬件。 */
int main(void)
{
  setvbuf(stdout,NULL,_IONBF,0U);
  assert(UartChannel_Init() == HAL_OK);
  AtCommand_Init();
  TestSchema();
  TestClock();
  TestAdmission();
  TestArbitration();
  TestSampling();
  TestPersistence();
  TestPlans();
  TestStorageFailure();
  TestStopWait();
  TestFragments();
  TestRaw();
  TestStatusPublishing();
  TestSntpIntegration();
  TestResumeCache();
  TestStatusPackets();
  puts("ALL REMOTE TESTS PASSED");
  return 0;
}
