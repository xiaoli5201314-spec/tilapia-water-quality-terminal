/* 文件：remote_control.c */
/* 功能：严格命令准入、停止优先、统一机械仲裁、自主计划及同快照四分片状态。 */
#include "remote_control.h"
#include "control_protocol.h"
#include "control_clock.h"
#include "cleaning_controller.h"
#include "actuator.h"
#include "brush_motor.h"
#include "alarm_light.h"
#include "at_command.h"
#include "network_manager.h"
#include "mqtt_transport.h"
#include "water_reporter.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define COMMAND_QUEUE_SIZE 8U
#define DEDUP_SIZE 128U
typedef struct {
  ControlCommand command;
  char id[13];
  bool valid, admitted_connection;
  uint32_t received, connection;
} Queued;
typedef struct { char id[13]; uint32_t tick; } Dedup;
typedef struct { uint32_t next_tick, last_day; } Schedule;
static ControlConfig config;
static ControlConfig persist_before;
static char persist_id[13];
static bool persist_safe_stop;
static Queued queue[COMMAND_QUEUE_SIZE], priority_stop;
static uint32_t queue_head, queue_count;
static bool stop_pending, initialized, connected, dirty, persist_pending;
static Dedup ledger[DEDUP_SIZE];
static Schedule brush_schedule, actuator_schedule;
static uint32_t cleaning_next, generation_seen, previous_unix, connection_epoch, connection_unix;
static uint32_t last_status, status_sequence, boot_id;
static CleaningState previous_cleaning;
static ActuatorState previous_actuator;
static bool previous_brush, previous_active, previous_synced;
static char device_id[25], command_topic[80], status_topic[80], ack_id[13], ack_message[40];
static bool ack_rejected;
static char parts[CONTROL_STATUS_PARTS][CONTROL_STATUS_CAPACITY];

/* 所有机械共享占用条件，自动计划与完整清洗不能同时抢占输出。 */
static bool MechanicalBusy(void)
{ return CleaningController_IsBusy() || Actuator_IsBusy() || BrushMotor_IsRunning(); }
/* 清除当前流程与全部待换向动作，不改变传感器电源。 */
static void CancelMotion(void)
{ CleaningController_Cancel(); Actuator_Stop(); BrushMotor_Stop(); }
/* 重建相对计划基准，不补做旧动作；日历已执行日期保持以防回拨重做。 */
static void Rearm(void)
{
  uint32_t now=HAL_GetTick();
  brush_schedule.next_tick=now+config.brush_plan.interval*1000U;
  actuator_schedule.next_tick=now+config.actuator_plan.interval*1000U;
  cleaning_next=now+config.cleaning.interval*1000U;
  previous_unix=ControlClock_Unix();
}
/* 回执只存可信 ID 和内部固定文本，JSON 不插入外部字符串。 */
static void Ack(const char *id, bool rejected, const char *message)
{
  memcpy(ack_id,id,13U);
  (void)snprintf(ack_message,sizeof(ack_message),"%s",message);
  ack_rejected=rejected; dirty=true;
}
/* 35秒有效窗口覆盖 TTL 与未来偏差；满表拒绝而非淘汰尚有效 ID。 */
static bool Remember(const char *id)
{
  uint32_t i, empty=DEDUP_SIZE, now=HAL_GetTick();
  for (i=0U;i<DEDUP_SIZE;++i)
  {
    if (ledger[i].id[0]!='\0' && now-ledger[i].tick>=35000U) { ledger[i].id[0]='\0'; }
    if (strcmp(ledger[i].id,id)==0) { return false; }
    if (ledger[i].id[0]=='\0' && empty==DEDUP_SIZE) { empty=i; }
  }
  if (empty==DEDUP_SIZE) { return false; }
  memcpy(ledger[empty].id,id,13U); ledger[empty].tick=now; return true;
}
/* 无同步仍可停止/查询；已同步时所有命令都受 TTL 和未来偏差约束。 */
static bool SafeCommand(const ControlCommand *c)
{
  return c->op==OP_STOP || c->op==OP_QUERY ||
    ((c->op==OP_BRUSH || c->op==OP_ACTUATOR || c->op==OP_CLEANING) && c->action==ACTION_STOP);
}
/* 只变更配置，不在 AT 回调/动作期间编程 Flash。 */
static void Changed(void)
{
  persist_pending=true; dirty=true;
  CleaningController_Configure(&config.cleaning);
  Rearm();
}
/* 单条业务执行，执行前已通过格式、时间、去重和连接世代校验。 */
static bool Apply(const ControlCommand *c)
{
  ControlConfig before=config;
  HAL_StatusTypeDef result=HAL_OK;
  bool ok=true;
  switch (c->op)
  {
    case OP_SAMPLING:
      config.collection_mode=c->mode; config.start=c->start; config.end=c->end;
      config.sample=c->sample; config.publish=c->publish;
      break;
    case OP_MODE:
      CancelMotion(); config.manual=c->mode; Rearm(); break;
    case OP_BRUSH:
      if (c->action==ACTION_STOP)
      {
        if (CleaningController_IsBusy()) { CancelMotion(); config.manual=1U; }
        else { BrushMotor_Stop(); }
      }
      else if (config.manual==0U || MechanicalBusy()) { ok=false; }
      else { result=BrushMotor_Start(c->duration*1000U); }
      break;
    case OP_ACTUATOR:
      if (c->action==ACTION_STOP)
      {
        if (CleaningController_IsBusy()) { CancelMotion(); config.manual=1U; }
        else { Actuator_Stop(); }
      }
      else if (config.manual==0U || CleaningController_IsBusy() || BrushMotor_IsRunning())
      { ok=false; }
      else { result=Actuator_Move(c->action==ACTION_EXTEND ? ACTUATOR_FORWARD : ACTUATOR_REVERSE,c->duration*1000U); }
      break;
    case OP_PLAN:
    {
      ControlPlan *p=c->target==0U ? &config.brush_plan : &config.actuator_plan;
      p->type=c->type; p->start=c->start; p->interval=c->interval;
      p->duration=c->duration; p->action=c->action; break;
    }
    case OP_CLEANING_CONFIG:
      config.cleaning.enabled=c->enabled; config.cleaning.interval=c->interval;
      config.cleaning.extend=c->extend; config.cleaning.brush=c->brush;
      config.cleaning.retract=c->retract; config.cleaning.settle=c->settle; break;
    case OP_CLEANING:
      if (c->action==ACTION_STOP) { CancelMotion(); config.manual=1U; }
      else if (config.manual!=0U || MechanicalBusy()) { ok=false; }
      else { ok=CleaningController_RequestStart()==CLEANING_STARTED; }
      break;
    case OP_STOP:
      CancelMotion(); config.manual=1U; Rearm(); break;
    case OP_QUERY: break;
    default: ok=false; break;
  }
  if (memcmp(&before,&config,sizeof(config))!=0)
  {
    persist_before=before;
    memcpy(persist_id,c->id,13U);
    persist_safe_stop=c->op==OP_STOP || (c->op==OP_CLEANING && c->action==ACTION_STOP) ||
      ((c->op==OP_BRUSH || c->op==OP_ACTUATOR) && c->action==ACTION_STOP);
    Changed();
  }
  return ok && result==HAL_OK;
}
/* 消费队列时再校验期限和世代，串口等待期间入队的旧动作不能迟到执行。 */
static void Process(const Queued *q)
{
  uint32_t now=ControlClock_Unix();
  const ControlCommand *c=&q->command;
  if (!q->valid) { if (q->id[0]!='\0') { Ack(q->id,true,"invalid"); } return; }
  if (!Remember(c->id)) { Ack(c->id,true,"duplicate"); return; }
  if (HAL_GetTick()-q->received>=30000U ||
      (ControlClock_IsSynced() && now>=c->issued && now-c->issued>=30U))
  { Ack(c->id,true,"expired"); return; }
  if (ControlClock_IsSynced() && c->issued>now && c->issued-now>5U)
  { Ack(c->id,true,"future"); return; }
  if (persist_pending && !SafeCommand(c)) { Ack(c->id,true,"busy"); return; }
  if (!SafeCommand(c))
  {
    if (!ControlClock_IsSynced()) { Ack(c->id,true,"clock_not_ready"); return; }
    if (!q->admitted_connection || !connected || q->connection!=connection_epoch ||
        c->issued<connection_unix)
    { Ack(c->id,true,"expired"); return; }
    if ((c->op==OP_SAMPLING || c->op==OP_PLAN || c->op==OP_CLEANING_CONFIG) && MechanicalBusy())
    { Ack(c->id,true,"busy"); return; }
  }
  if ((c->op==OP_BRUSH || c->op==OP_ACTUATOR) && c->action!=ACTION_STOP && config.manual==0U)
  { Ack(c->id,true,"manual_required"); return; }
  if (c->op==OP_CLEANING && c->action==ACTION_START && config.manual!=0U)
  { Ack(c->id,true,"auto_required"); return; }
  if (Apply(c))
  {
    if (!persist_pending) { Ack(c->id,false,""); }
  }
  else { Ack(c->id,true,"busy"); }
}
/* 初始化安全输出并读取配置，运行状态和手动动作不从 Flash 恢复。 */
void RemoteControl_Init(void)
{
  CancelMotion();
  (void)ControlConfig_Load(&config);
  CleaningController_Configure(&config.cleaning);
  memset(ledger,0,sizeof(ledger));
  queue_head=0U; queue_count=0U; stop_pending=false;
  connected=false; connection_epoch=0U; connection_unix=0U;
  dirty=true; persist_pending=false; ack_id[0]='\0'; ack_message[0]='\0'; ack_rejected=false;
  brush_schedule.last_day=actuator_schedule.last_day=UINT32_MAX;
  generation_seen=ControlClock_Generation();
  status_sequence=0U; last_status=HAL_GetTick()-5000U;
  /* F407 已有 RNG 外设可直接取随机启动标识；主机用固定可重现替身。 */
#ifdef CONTROL_HOST_TEST
  boot_id=0x12345678U;
#else
  __HAL_RCC_RNG_CLK_ENABLE();
  RNG->CR=RNG_CR_RNGEN;
  {
    uint32_t attempt=0U;
    while ((RNG->SR&RNG_SR_DRDY)==0U && ++attempt<1000000U) {}
    boot_id=(RNG->SR&RNG_SR_DRDY)!=0U ? RNG->DR :
      HAL_GetUIDw0()^HAL_GetUIDw1()^HAL_GetTick()^RNG->SR;
  }
  RNG->CR=0U; __HAL_RCC_RNG_CLK_DISABLE();
#endif
  (void)snprintf(device_id,sizeof(device_id),"%08lX%08lX%08lX",
    (unsigned long)HAL_GetUIDw0(),(unsigned long)HAL_GetUIDw1(),(unsigned long)HAL_GetUIDw2());
  (void)snprintf(command_topic,sizeof(command_topic),"fish/device/%s/water/cmd",device_id);
  (void)snprintf(status_topic,sizeof(status_topic),"fish/device/%s/water/control/status",device_id);
  previous_cleaning=CLEANING_IDLE; previous_actuator=ACTUATOR_STOP;
  previous_brush=false; previous_active=false; previous_synced=false;
  initialized=true; Rearm();
}
/* 完整 URC 只解析入队，停止有独立槽并优先于普通动作。 */
void RemoteControl_Receive(const char *topic, const uint8_t *payload, uint16_t length)
{
  Queued q;
  if (!initialized || topic==NULL || strcmp(topic,command_topic)!=0) { return; }
  memset(&q,0,sizeof(q));
  q.valid=ControlProtocol_Parse(payload,length,&q.command,q.id);
  q.received=HAL_GetTick(); q.connection=connection_epoch; q.admitted_connection=connected;
  if (q.valid && SafeCommand(&q.command) && q.command.op!=OP_QUERY)
  {
    /* 首个停止不可被重复/无效后续停止覆盖。 */
    if (!stop_pending) { priority_stop=q; stop_pending=true; }
    return;
  }
  if (queue_count>=COMMAND_QUEUE_SIZE)
  { if (q.id[0]!='\0') { Ack(q.id,true,"busy"); } return; }
  queue[(queue_head+queue_count)%COMMAND_QUEUE_SIZE]=q; ++queue_count;
}
/* 连接断开不停止已认可的自主计划；取消旧连接的排队动作并要求恢复后新命令。 */
void RemoteControl_SetConnected(bool up)
{
  if (!initialized || connected==up) { return; }
  connected=up; ++connection_epoch; connection_unix=ControlClock_Unix();
  queue_count=0U; dirty=true;
  ControlClock_LinkChanged(up);
}
/* 日历计划只在正常连续秒推进且新分钟边界触发，错过即跳过。 */
static bool Due(const ControlPlan *p, Schedule *s, uint32_t now, uint32_t old)
{
  uint32_t day=(now+28800U)/86400U, minute=((now+28800U)/60U)%1440U;
  if (p->type==PLAN_OFF) { return false; }
  if (p->type==PLAN_INTERVAL)
  {
    if ((int32_t)(HAL_GetTick()-s->next_tick)<0) { return false; }
    s->next_tick=HAL_GetTick()+p->interval*1000U; return true;
  }
  if (now<=old || now-old>2U || now/60U==old/60U || minute!=p->start ||
      (s->last_day!=UINT32_MAX && day<=s->last_day)) { return false; }
  s->last_day=day; return true;
}
/* 非阻塞推进命令和所有机械状态，AT/RS485 等待期间也可调用。 */
void RemoteControl_Poll(void)
{
  uint32_t now, gen;
  bool brush_due, actuator_due;
  if (!initialized) { return; }
  Actuator_Task(); BrushMotor_Task(); CleaningController_Task();
  if (stop_pending)
  {
    Queued q=priority_stop; stop_pending=false;
    Process(&q);
    if (!ack_rejected) { queue_count=0U; }
  }
  while (queue_count!=0U)
  {
    Queued q=queue[queue_head]; queue_head=(queue_head+1U)%COMMAND_QUEUE_SIZE; --queue_count;
    Process(&q);
  }
  gen=ControlClock_Generation(); now=ControlClock_Unix();
  if (gen!=generation_seen)
  {
    generation_seen=gen;
    if (connected && connection_unix==0U && ControlClock_IsSynced()) { connection_unix=now; }
    Rearm(); dirty=true;
  }
  if (ControlClock_IsSynced() && config.manual==0U)
  {
    brush_due=Due(&config.brush_plan,&brush_schedule,now,previous_unix);
    actuator_due=Due(&config.actuator_plan,&actuator_schedule,now,previous_unix);
    if (config.cleaning.enabled!=0U && (int32_t)(HAL_GetTick()-cleaning_next)>=0)
    {
      cleaning_next=HAL_GetTick()+config.cleaning.interval*1000U;
      if (!MechanicalBusy()) { (void)CleaningController_RequestStart(); }
    }
    if (brush_due && !MechanicalBusy()) { (void)BrushMotor_Start(config.brush_plan.duration*1000U); }
    if (actuator_due && !MechanicalBusy())
    { (void)Actuator_Move(config.actuator_plan.action==ACTION_EXTEND ? ACTUATOR_FORWARD : ACTUATOR_REVERSE,
                          config.actuator_plan.duration*1000U); }
  }
  previous_unix=now;
  /* AT 等待可能完成整次清洗，必须在暂停发生时通知采集层清除旧测量。 */
  if (!RemoteControl_CollectionActive()) { WaterReporter_Invalidate(); }
  if (previous_cleaning!=CleaningController_GetState())
  {
    if (previous_cleaning==CLEANING_SETTLING && CleaningController_GetState()==CLEANING_IDLE)
    { cleaning_next=HAL_GetTick()+config.cleaning.interval*1000U; }
    previous_cleaning=CleaningController_GetState(); dirty=true;
  }
  if (previous_actuator!=Actuator_GetState() || previous_brush!=BrushMotor_IsRunning() ||
      previous_active!=RemoteControl_CollectionActive() || previous_synced!=ControlClock_IsSynced())
  {
    previous_actuator=Actuator_GetState(); previous_brush=BrushMotor_IsRunning();
    previous_active=RemoteControl_CollectionActive(); previous_synced=ControlClock_IsSynced(); dirty=true;
  }
}
/* 仅前台空闲保存实际变更；失败明确拒绝持久化，保留安全停止的 RAM 模式。 */
void RemoteControl_PersistTask(void)
{
  if (!persist_pending || AtCommand_IsBusy() || MechanicalBusy()) { return; }
  persist_pending=false;
  if (!ControlConfig_Save(&config))
  {
    if (!persist_safe_stop) { config=persist_before; CleaningController_Configure(&config.cleaning); Rearm(); }
    Ack(persist_id,true,"storage_error");
  }
  else { Ack(persist_id,false,""); }
}
/* 采集时间开始含/结束不含；跨午夜单独处理，动作与稳定等待始终暂停读取。 */
bool RemoteControl_CollectionActive(void)
{
  uint32_t minute;
  if (!initialized || MechanicalBusy() || config.collection_mode==COLLECTION_PAUSE) { return false; }
  if (config.collection_mode==COLLECTION_RUN) { return true; }
  if (!ControlClock_IsSynced()) { return false; }
  minute=((ControlClock_Unix()+28800U)/60U)%1440U;
  return config.start<config.end ? minute>=config.start && minute<config.end :
    minute>=config.start || minute<config.end;
}
/* 暴露只读配置和回执状态，不允许外部写内部计划。 */
const ControlConfig *RemoteControl_Config(void) { return &config; }
/* 返回绑定设备 UID 的命令 Topic。 */
const char *RemoteControl_CommandTopic(void) { return command_topic; }
/* 四片共享同一个控制状态 Topic。 */
const char *RemoteControl_StatusTopic(void) { return status_topic; }
/* 测试与业务只读当前回执结果。 */
bool RemoteControl_AckRejected(void) { return ack_rejected; }
/* 紧急停止进入手动，后续自动计划不会重启机械。 */
void RemoteControl_EmergencyStop(void)
{
  persist_before=config; persist_id[0]='\0'; persist_safe_stop=true;
  CancelMotion(); config.manual=1U; Changed();
  AlarmLight_Set(ALARM_LIGHT_RED);
}

/* 计划结构严格按外部契约编码，不增加位置和转速伪测量。 */
static int PlanJson(char *out, size_t capacity, const ControlPlan *p)
{
  static const char *const types[]={"off","daily","interval"};
  static const char *const actions[]={"stop","start","extend","retract"};
  return snprintf(out,capacity,"{\"type\":\"%s\",\"start\":%lu,\"interval\":%lu,\"duration\":%lu,\"action\":\"%s\"}",
    types[p->type],(unsigned long)p->start,(unsigned long)p->interval,(unsigned long)p->duration,actions[p->action]);
}
/* 捕获一份一致状态并生成全部四片，网络发布期间不重新读取运行状态。 */
bool RemoteControl_CaptureStatus(uint32_t sequence_value)
{
  char group[4][290], bp[112], ap[112];
  int n[4], i, envelope;
  uint32_t now=ControlClock_Unix();
  bool clock_synced=ControlClock_IsSynced();
  static const char *const modes[]={"run","pause","schedule"};
  static const char *const cleaning_states[]={"idle","extending","brushing","retracting","settling","fault"};
  static const char *const lights[]={"off","red","yellow","green"};
  const char *actuator_state=Actuator_GetState()==ACTUATOR_FORWARD ? "extending" :
    (Actuator_GetState()==ACTUATOR_REVERSE ? "retracting" : (Actuator_IsBusy() ? "waiting" : "stopped"));
  uint32_t next=(config.manual!=0U || config.cleaning.enabled==0U || !clock_synced || CleaningController_IsBusy()) ? 0U :
    ((int32_t)(HAL_GetTick()-cleaning_next)>=0 ? 0U : (cleaning_next-HAL_GetTick()+999U)/1000U);
  if (!ControlConfig_Valid(&config)) { return false; }
  n[0]=snprintf(group[0],sizeof(group[0]),
    "\"controlMode\":\"%s\",\"collection\":{\"mode\":\"%s\",\"active\":%s,\"start\":%lu,\"end\":%lu,\"sample\":%lu,\"publish\":%lu},"
    "\"clock\":{\"synced\":%s,\"unix\":%lu},\"light\":\"%s\",\"uptimeMs\":%lu",
    config.manual ? "manual":"auto",modes[config.collection_mode],RemoteControl_CollectionActive() ? "true":"false",
    (unsigned long)config.start,(unsigned long)config.end,(unsigned long)config.sample,(unsigned long)config.publish,
    clock_synced ? "true":"false",(unsigned long)now,lights[AlarmLight_Get()],(unsigned long)HAL_GetTick());
  n[1]=snprintf(group[1],sizeof(group[1]),
    "\"brush\":{\"running\":%s,\"remaining\":%lu},\"actuator\":{\"state\":\"%s\",\"remaining\":%lu},"
    "\"cleaning\":{\"state\":\"%s\",\"nextIn\":%lu},\"ack\":{\"id\":\"%s\",\"result\":\"%s\",\"message\":\"%s\"}",
    BrushMotor_IsRunning() ? "true":"false",(unsigned long)BrushMotor_Remaining(),
    actuator_state,(unsigned long)Actuator_Remaining(),cleaning_states[CleaningController_GetState()],
    (unsigned long)next,ack_id,ack_rejected ? "rejected":"ok",ack_message);
  if (PlanJson(bp,sizeof(bp),&config.brush_plan)<=0 || PlanJson(ap,sizeof(ap),&config.actuator_plan)<=0) { return false; }
  n[2]=snprintf(group[2],sizeof(group[2]),"\"brushPlan\":%s,\"actuatorPlan\":%s",bp,ap);
  n[3]=snprintf(group[3],sizeof(group[3]),
    "\"cleaningConfig\":{\"enabled\":%s,\"interval\":%lu,\"extend\":%lu,\"brush\":%lu,\"retract\":%lu,\"settle\":%lu}",
    config.cleaning.enabled ? "true":"false",(unsigned long)config.cleaning.interval,
    (unsigned long)config.cleaning.extend,(unsigned long)config.cleaning.brush,
    (unsigned long)config.cleaning.retract,(unsigned long)config.cleaning.settle);
  for (i=0;i<4;++i)
  {
    if (n[i]<=0 || (size_t)n[i]>=sizeof(group[i])) { return false; }
    envelope=snprintf(parts[i],sizeof(parts[i]),
      "{\"v\":1,\"deviceId\":\"%s\",\"online\":true,\"bootId\":\"%08lX\",\"seq\":%lu,\"part\":%d,\"parts\":4,%s}",
      device_id,(unsigned long)boot_id,(unsigned long)sequence_value,i,group[i]);
    if (envelope<=0 || (unsigned int)envelope>=CONTROL_STATUS_CAPACITY ||
        (size_t)envelope+strlen(status_topic)+8U>512U) { return false; }
  }
  return true;
}
/* 返回已冻结的片正文，索引无效返回 NULL。 */
const char *RemoteControl_StatusPart(uint32_t part)
{ return part<CONTROL_STATUS_PARTS ? parts[part] : NULL; }
/* 每五秒或变更发布四片，每片只尝试一次，失败整快照作废，不补发旧状态。 */
void RemoteControl_StatusTask(void)
{
  uint32_t i;
  if (!connected || AtCommand_IsBusy() || NetworkManager_GetActiveLink()!=MQTT_LINK_WIFI ||
      (!dirty && HAL_GetTick()-last_status<5000U)) { return; }
  RemoteControl_PersistTask();
  if (persist_pending) { return; }
  last_status=HAL_GetTick(); dirty=false;
  if (!RemoteControl_CaptureStatus(++status_sequence)) { dirty=true; return; }
  for (i=0U;i<CONTROL_STATUS_PARTS;++i)
  {
    if (MqttTransport_PublishRaw(MQTT_LINK_WIFI,status_topic,(const uint8_t *)parts[i],
        (uint16_t)strlen(parts[i]))!=MQTT_RESULT_OK) { return; }
  }
}
