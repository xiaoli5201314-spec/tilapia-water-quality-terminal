/* 文件：control_clock.c */
/* 功能：校验 SNTP 日历并维护带失效期限的 UTC 时钟，跳变时重置计划基准。 */
#include "control_clock.h"
#include "at_command.h"
#include "board_config.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
static uint32_t unix_base, tick_base, age_ms, fraction_ms, last_tick, generation;
static uint32_t retry_at;
static bool synced, connected, configured;
static char response[160];
static const char *const months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static const char *const weekdays[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

/* 格里高利历闰年判断。 */
static bool Leap(uint32_t y)
{ return y%4U==0U && (y%100U!=0U || y%400U==0U); }
/* 输入北京时间，验证年月日、小时、星期和尾部，再转为 UTC。 */
bool ControlClock_Parse(const char *text, uint32_t *value)
{
  char week[4], month[4];
  unsigned int day,hour,minute,second,year;
  int used=0, m=-1, w=-1, i;
  uint32_t days=0U, y, maxday;
  static const uint32_t mdays[]={31U,28U,31U,30U,31U,30U,31U,31U,30U,31U,30U,31U};
  if (text==NULL || value==NULL ||
      sscanf(text,"%3s %3s %u %u:%u:%u %u%n",week,month,&day,&hour,&minute,&second,&year,&used)!=7)
  { return false; }
  if (text[used]!='\0' || year<2024U || year>2099U || hour>23U || minute>59U || second>59U)
  { return false; }
  for (i=0;i<12;++i) { if (strcmp(month,months[i])==0) { m=i; } }
  for (i=0;i<7;++i) { if (strcmp(week,weekdays[i])==0) { w=i; } }
  if (m<0 || w<0) { return false; }
  maxday=mdays[m]+(m==1 && Leap(year) ? 1U : 0U);
  if (day<1U || day>maxday) { return false; }
  for (y=1970U;y<year;++y) { days+=Leap(y) ? 366U : 365U; }
  for (i=0;i<m;++i) { days+=mdays[i]+(i==1 && Leap(year) ? 1U : 0U); }
  days+=day-1U;
  if ((days+4U)%7U!=(uint32_t)w) { return false; }
  *value=days*86400U+hour*3600U+minute*60U+second-28800U;
  return true;
}
/* 增量推进避免 49.7 天 tick 回绕丢时，最长一天未校时则取消同步。 */
static void Advance(void)
{
  uint32_t now=HAL_GetTick(), delta=now-last_tick;
  last_tick=now;
  if (!synced) { return; }
  if (delta>=86400000U || age_ms>=86400000U-delta) { synced=false; ++generation; return; }
  age_ms+=delta; fraction_ms+=delta;
  unix_base+=fraction_ms/1000U; fraction_ms%=1000U;
}
/* 上电没有有效日历，不恢复上次掉电前的墙钟。 */
void ControlClock_Init(void)
{
  unix_base=0U; tick_base=HAL_GetTick(); last_tick=tick_base;
  age_ms=0U; fraction_ms=0U; generation=0U; synced=false;
  connected=false; configured=false; retry_at=tick_base;
}
/* 校时超过两秒跳变或首次同步时增加计划世代，防止补执行。 */
void ControlClock_SetUnix(uint32_t value)
{
  uint32_t old=ControlClock_Unix();
  bool was=synced;
  if (!was || (value>old ? value-old : old-value)>2U) { ++generation; }
  unix_base=value; tick_base=HAL_GetTick(); last_tick=tick_base;
  age_ms=0U; fraction_ms=0U; synced=true;
}
/* 每次读取先推进时间，同步过期不能继续启动机械。 */
bool ControlClock_IsSynced(void) { Advance(); return synced; }
/* 未同步时回传零，避免把 uptime 误称 UTC。 */
uint32_t ControlClock_Unix(void) { Advance(); return synced ? unix_base : 0U; }
/* 调度器据此清除旧的日历触发基准。 */
uint32_t ControlClock_Generation(void) { Advance(); return generation; }
/* 连接恢复触发重新校时，离线保留一天内有效时钟供自主计划使用。 */
void ControlClock_LinkChanged(bool up)
{
  connected=up;
  if (up) { configured=false; retry_at=HAL_GetTick(); ++generation; }
}
/* 一次配置或查询一条 AT，失败十秒重试，成功一小时复校。 */
void ControlClock_Task(void)
{
  char *p, *end;
  uint32_t value;
  Advance();
  if (!connected || AtCommand_IsBusy() || (int32_t)(HAL_GetTick()-retry_at)<0) { return; }
  retry_at=HAL_GetTick()+10000U;
  if (!configured)
  {
    configured=AtCommand_Execute(BOARD_WIFI_UART,
      "AT+CIPSNTPCFG=1,8,\"ntp.aliyun.com\",\"pool.ntp.org\"",
      response,sizeof(response),2000U)==HAL_OK;
    return;
  }
  if (AtCommand_Execute(BOARD_WIFI_UART,"AT+CIPSNTPTIME?",response,sizeof(response),2000U)!=HAL_OK)
  { return; }
  p=strstr(response,"+CIPSNTPTIME:");
  if (p==NULL) { return; }
  p+=strlen("+CIPSNTPTIME:");
  end=strpbrk(p,"\r\n");
  if (end!=NULL) { *end='\0'; }
  if (!ControlClock_Parse(p,&value)) { return; }
  ControlClock_SetUnix(value);
  retry_at=HAL_GetTick()+3600000U;
}
