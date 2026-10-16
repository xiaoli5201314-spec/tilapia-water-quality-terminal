/* 文件：at_stream.c */
/* 功能：持续保存分片帧，分离 MQTT 正文、AT 完整行和 RAW 提示符。 */
#include "at_stream.h"
#include "board_config.h"
#include "control_protocol.h"
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#define STREAM_LINE_CAPACITY 384U
typedef enum { STREAM_LINE, STREAM_HEADER, STREAM_PAYLOAD, STREAM_DROP_LINE } StreamMode;
typedef struct {
  StreamMode mode;
  char line[STREAM_LINE_CAPACITY], topic[129];
  uint8_t payload[CONTROL_COMMAND_MAX];
  uint16_t used;
  uint32_t expected, received;
  bool discard;
} Stream;
static Stream streams[2];
static AtStreamLineCallback line_callback;
static AtStreamMessageCallback message_callback;

/* 两路网络串口各保留独立状态，不读取 Modbus 串口。 */
static Stream *Find(UART_HandleTypeDef *uart)
{
  if (uart==BOARD_WIFI_UART) { return &streams[0]; }
  if (uart==BOARD_4G_UART) { return &streams[1]; }
  return NULL;
}
/* 上电清零，运行中的 AT 事务不调用此函数。 */
void AtStream_Init(void)
{
  memset(streams,0,sizeof(streams));
  line_callback=NULL; message_callback=NULL;
}
/* 回调在前台运行，消息回调只入队，不重入 AT。 */
void AtStream_SetCallbacks(AtStreamLineCallback line, AtStreamMessageCallback message)
{ line_callback=line; message_callback=message; }
/* 识别 MQTT URC 的 link/topic/length 头，长度检查避免溢出。 */
static bool Header(Stream *s)
{
  const char *p=s->line+13;
  uint16_t topic_length=0U;
  uint32_t length=0U;
  if (strncmp(s->line,"+MQTTSUBRECV:",13U)!=0 || *p++!='0' || *p++!=',' || *p++!='"')
  { return false; }
  while (*p!='\0' && *p!='"')
  {
    if (topic_length>=128U || (unsigned char)*p<32U) { return false; }
    s->topic[topic_length++]=*p++;
  }
  if (*p++!='"' || *p++!=',' || *p<'0' || *p>'9') { return false; }
  while (*p>='0' && *p<='9')
  {
    if (length>(65535U-(uint32_t)(*p-'0'))/10U) { return false; }
    length=length*10U+(uint32_t)(*p++-'0');
  }
  if (*p++!=',' || *p!='\0') { return false; }
  s->topic[topic_length]='\0'; s->expected=length; s->received=0U;
  s->discard=(length==0U || length>CONTROL_COMMAND_MAX);
  return true;
}
/* 溢出损坏的当前命令不得分发，直到新完整行边界才重新识别。 */
void AtStream_Fault(UART_HandleTypeDef *uart)
{
  Stream *s=Find(uart);
  if (s!=NULL) { s->mode=STREAM_DROP_LINE; s->used=0U; }
}
/* 按声明长度消费正文，正文中的 OK、换行与逗号不会污染 AT 事务。 */
void AtStream_Feed(UART_HandleTypeDef *uart, uint8_t byte)
{
  Stream *s=Find(uart);
  if (s==NULL) { return; }
  if (s->mode==STREAM_PAYLOAD)
  {
    if (!s->discard) { s->payload[s->received]=byte; }
    if (++s->received==s->expected)
    {
      s->mode=STREAM_LINE; s->used=0U;
      if (!s->discard && message_callback!=NULL)
      { message_callback(s->topic,s->payload,(uint16_t)s->expected); }
    }
    return;
  }
  if (s->mode==STREAM_DROP_LINE)
  {
    if (byte=='\n') { s->mode=STREAM_LINE; s->used=0U; }
    return;
  }
  if (s->mode==STREAM_LINE && byte=='>' && s->used==0U)
  {
    if (line_callback!=NULL) { line_callback(uart,">"); }
    return;
  }
  if (byte=='\n')
  {
    if (s->mode==STREAM_LINE && s->used!=0U)
    {
      s->line[s->used]='\0';
      if (line_callback!=NULL) { line_callback(uart,s->line); }
    }
    s->used=0U; s->mode=STREAM_LINE; return;
  }
  if (byte=='\r') { return; }
  if (byte==0U || s->used+1U>=STREAM_LINE_CAPACITY)
  { AtStream_Fault(uart); return; }
  s->line[s->used++]=(char)byte; s->line[s->used]='\0';
  if (s->used==13U && strcmp(s->line,"+MQTTSUBRECV:")==0)
  { s->mode=STREAM_HEADER; }
  if (s->mode==STREAM_HEADER && byte==',' && Header(s))
  {
    s->mode=s->expected==0U ? STREAM_LINE : STREAM_PAYLOAD;
    s->used=0U;
  }
}
