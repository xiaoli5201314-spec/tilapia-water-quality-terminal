/* 文件：at_command.c */
/* 功能：持续解复用 AT/URC，同步事务等待时维护 GPIO 与前台控制，不清空 UART。 */
#include "at_command.h"
#include "at_stream.h"
#include "uart_channel.h"
#include "board_config.h"
#include "actuator.h"
#include "brush_motor.h"
#include "remote_control.h"
#include "network_manager.h"
#include <stddef.h>
#include <string.h>
#define AT_COMMAND_SIZE 384U
static bool busy, polling, got_ok, got_event, got_prompt, failed, overflow;
static UART_HandleTypeDef *waiting_uart;
static const char *waiting_event;
static char *waiting_response;
static uint16_t response_capacity, response_used;
/* 调试器可读取失败阶段、耗时和真实最后应答；不保存 AT 配置或密码。 */
volatile AtRawTrace g_at_raw_trace;
static bool tracing_raw;
static uint32_t raw_started;

/* 完整行匹配终结应答，不从数据正文搜索 OK。 */
HAL_StatusTypeDef AtCommand_ClassifyLine(const char *line)
{
  if (line==NULL) { return HAL_ERROR; }
  if (strcmp(line,"OK")==0) { return HAL_OK; }
  if (strcmp(line,"ERROR")==0 || strcmp(line,"FAIL")==0 ||
      strcmp(line,"CONNECT FAIL")==0 || strcmp(line,"CONNACK FAIL")==0 ||
      strcmp(line,"+MQTTPUB:FAIL")==0 || strncmp(line,"busy ",5U)==0 ||
      strncmp(line,"+CME ERROR:",11U)==0 || strncmp(line,"+CMS ERROR:",11U)==0)
  { return HAL_ERROR; }
  return HAL_BUSY;
}
/* URC 立即标记断链；其余完整行仅交给当前事务，空闲应答不确认下一条命令。 */
static void Line(UART_HandleTypeDef *uart, const char *line)
{
  size_t length;
  if (uart==BOARD_WIFI_UART &&
      (strcmp(line,"WIFI DISCONNECT")==0 || strcmp(line,"+MQTTDISCONNECTED:0")==0 ||
       strcmp(line,"ready")==0))
  { NetworkManager_NotifyDisconnected(); }
  if (!busy || uart!=waiting_uart) { return; }
  if (tracing_raw)
  {
    size_t i;
    for (i=0U;i+1U<sizeof(g_at_raw_trace.last_line) && line[i]!='\0';++i)
    { g_at_raw_trace.last_line[i]=line[i]; }
    g_at_raw_trace.last_line[i]='\0';
  }
  if (strcmp(line,">")==0) { got_prompt=true; return; }
  if (AtCommand_ClassifyLine(line)==HAL_ERROR) { failed=true; }
  if (strcmp(line,"OK")==0) { got_ok=true; }
  if (waiting_event!=NULL && strcmp(line,waiting_event)==0) { got_event=true; }
  length=strlen(line);
  if (waiting_response!=NULL)
  {
    if (response_used+length+2U>=response_capacity) { overflow=true; return; }
    memcpy(waiting_response+response_used,line,length); response_used+=(uint16_t)length;
    waiting_response[response_used++]='\r'; waiting_response[response_used++]='\n';
    waiting_response[response_used]='\0';
  }
}
/* 上电建立解析回调，消息只进入远程控制队列。 */
void AtCommand_Init(void)
{
  busy=false; polling=false; waiting_uart=NULL;
  tracing_raw=false;
  g_at_raw_trace.stage=AT_RAW_IDLE;
  AtStream_Init(); AtStream_SetCallbacks(Line,RemoteControl_Receive);
}
/* 在空闲、AT 与 RS485 等待中复用；解析回调不能启动新的 AT。 */
void AtCommand_Poll(void)
{
  uint8_t byte;
  uint32_t count=0U;
  UART_HandleTypeDef *uarts[]={BOARD_WIFI_UART,BOARD_4G_UART};
  unsigned int i;
  if (polling) { return; }
  polling=true;
  UartChannel_Task();
  for (i=0U;i<2U;++i)
  {
    if (UartChannel_TakeError(uarts[i]))
    {
      AtStream_Fault(uarts[i]);
      if (busy && waiting_uart==uarts[i]) { failed=true; }
      if (i==0U) { NetworkManager_NotifyDisconnected(); }
    }
    while (count<2048U && UartChannel_ReadByte(uarts[i],&byte))
    { AtStream_Feed(uarts[i],byte); ++count; }
  }
  Actuator_Task(); BrushMotor_Task(); RemoteControl_Poll();
  polling=false;
}
/* 事务占用表示 AT 前台不可重入，不限制机械运行期间状态发布。 */
bool AtCommand_IsBusy(void) { return busy; }
/* 结束事务保留流解析器与尚未读取的 URC。 */
static HAL_StatusTypeDef Finish(HAL_StatusTypeDef result)
{
  if (tracing_raw)
  {
    g_at_raw_trace.result=result;
    g_at_raw_trace.elapsed_ms=HAL_GetTick()-raw_started;
    if (result==HAL_OK) { g_at_raw_trace.stage=AT_RAW_DONE; }
    tracing_raw=false;
  }
  busy=false; waiting_uart=NULL; waiting_event=NULL; waiting_response=NULL;
  return result;
}
/* 发送前先消费旧数据，避免已到达的旧 OK 满足新事务。 */
static HAL_StatusTypeDef Begin(UART_HandleTypeDef *uart, const char *command,
  const char *event, char *response, uint16_t capacity, uint32_t timeout)
{
  char tx[AT_COMMAND_SIZE];
  size_t length;
  HAL_StatusTypeDef result;
  if (busy) { return HAL_BUSY; }
  if (response!=NULL && capacity!=0U) { response[0]='\0'; }
  if (uart==NULL || command==NULL || timeout==0U || timeout==HAL_MAX_DELAY ||
      (response!=NULL && capacity<2U) || (event!=NULL && event[0]=='\0'))
  { return HAL_ERROR; }
  length=strlen(command);
  if (length==0U || length+2U>=sizeof(tx) || strpbrk(command,"\r\n")!=NULL)
  { return HAL_ERROR; }
  AtCommand_Poll();
  busy=true; waiting_uart=uart; waiting_event=event;
  waiting_response=response; response_capacity=capacity; response_used=0U;
  got_ok=false; got_event=(event==NULL); got_prompt=false; failed=false; overflow=false;
  memcpy(tx,command,length); tx[length++]='\r'; tx[length++]='\n';
  result=UartChannel_Write(uart,(const uint8_t *)tx,(uint16_t)length,timeout);
  if (result!=HAL_OK) { return Finish(result); }
  return HAL_OK;
}
/* 等待 OK 和指定完整异步行，期间每毫秒维护控制。 */
HAL_StatusTypeDef AtCommand_ExecuteUntil(UART_HandleTypeDef *uart, const char *command,
  const char *event, char *response, uint16_t capacity, uint32_t timeout)
{
  uint32_t start=HAL_GetTick();
  HAL_StatusTypeDef result;
  if (response==NULL) { return HAL_ERROR; }
  result=Begin(uart,command,event,response,capacity,timeout);
  if (result!=HAL_OK) { return result; }
  while (HAL_GetTick()-start<timeout)
  {
    AtCommand_Poll();
    if (failed || overflow) { return Finish(HAL_ERROR); }
    if (got_ok && got_event) { return Finish(HAL_OK); }
    HAL_Delay(1U);
  }
  return Finish(HAL_TIMEOUT);
}
/* 普通 AT 必须收到完整 OK 终结行。 */
HAL_StatusTypeDef AtCommand_Execute(UART_HandleTypeDef *uart, const char *command,
  char *response, uint16_t capacity, uint32_t timeout)
{ return AtCommand_ExecuteUntil(uart,command,NULL,response,capacity,timeout); }
/* RAW 必须先收到提示符、再发精确长度、最后收到发布成功 URC。 */
HAL_StatusTypeDef AtCommand_PublishRaw(UART_HandleTypeDef *uart, const char *command,
  const uint8_t *payload, uint16_t length, uint32_t timeout)
{
  HAL_StatusTypeDef result;
  uint32_t start=HAL_GetTick();
  uint16_t sent=0U;
  if (payload==NULL || length==0U) { return HAL_ERROR; }
  if (busy) { return HAL_BUSY; }
  raw_started=start;
  g_at_raw_trace.stage=AT_RAW_PROMPT;
  g_at_raw_trace.result=HAL_BUSY;
  g_at_raw_trace.expected_bytes=length;
  g_at_raw_trace.sent_bytes=0U;
  g_at_raw_trace.last_line[0]='\0';
  tracing_raw=true;
  result=Begin(uart,command,"+MQTTPUB:OK",NULL,0U,timeout);
  if (result!=HAL_OK) { return Finish(result); }
  while (HAL_GetTick()-start<timeout && !got_prompt)
  {
    AtCommand_Poll();
    if (failed) { return Finish(HAL_ERROR); }
    if (!got_prompt) { HAL_Delay(1U); }
  }
  if (!got_prompt) { return Finish(HAL_TIMEOUT); }
  got_event=false;
  /* 提示符一旦到达就完整发送声明正文，不让等待提示符耗掉正文发送预算。 */
  start=HAL_GetTick();
  g_at_raw_trace.stage=AT_RAW_PAYLOAD;
  while (sent<length)
  {
    uint16_t chunk=(uint16_t)(length-sent);
    if (chunk>32U) { chunk=32U; }
    if (HAL_GetTick()-start>=timeout) { return Finish(HAL_TIMEOUT); }
    /* 32 字节最多约 3 ms，发送间隙仍处理停止与超时。 */
    result=UartChannel_Write(uart,payload+sent,chunk,20U);
    if (result!=HAL_OK) { return Finish(result); }
    sent=(uint16_t)(sent+chunk);
    g_at_raw_trace.sent_bytes=sent;
    AtCommand_Poll();
    if (failed) { return Finish(HAL_ERROR); }
  }
  /* 发布成功应答从最后一字节发完后单独计时，不能沿用提示符等待起点。 */
  start=HAL_GetTick();
  g_at_raw_trace.stage=AT_RAW_ACK;
  while (HAL_GetTick()-start<timeout)
  {
    AtCommand_Poll();
    if (failed) { return Finish(HAL_ERROR); }
    if (got_event) { return Finish(HAL_OK); }
    HAL_Delay(1U);
  }
  return Finish(HAL_TIMEOUT);
}
