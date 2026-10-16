/* 文件：control_config.c */
/* 功能：配置默认值、语义边界、固定小端版本记录和软件 CRC32。 */
#include "control_config.h"
#include <stddef.h>
#include <string.h>
#define CONFIG_MAGIC 0x5743544CUL
#define CONFIG_VERSION 1U

/* 固定默认周期与原正式硬件流程保持一致。 */
void ControlConfig_Default(ControlConfig *c)
{
  if (c == NULL) { return; }
  memset(c, 0, sizeof(*c));
  c->collection_mode = COLLECTION_RUN;
  c->start = 480U; c->end = 1080U; c->sample = 5U; c->publish = 3600U;
  c->brush_plan.start = c->actuator_plan.start = 480U;
  c->brush_plan.interval = c->actuator_plan.interval = 21600U;
  c->brush_plan.duration = 10U; c->actuator_plan.duration = 12U;
  c->brush_plan.action = ACTION_START; c->actuator_plan.action = ACTION_EXTEND;
  c->cleaning.enabled = 1U; c->cleaning.interval = 21600U;
  c->cleaning.extend = 12U; c->cleaning.brush = 10U;
  c->cleaning.retract = 12U; c->cleaning.settle = 30U;
}

/* 每个独立计划都必须满足范围与目标对应动作，关闭计划也验证保存字段。 */
static bool PlanValid(const ControlPlan *p, bool brush)
{
  return p->type <= PLAN_INTERVAL && p->start <= 1439U &&
    p->interval >= 60U && p->interval <= 86400U &&
    p->duration >= 1U && p->duration <= 30U &&
    (brush ? p->action == ACTION_START :
      (p->action == ACTION_EXTEND || p->action == ACTION_RETRACT));
}

/* 禁止损坏配置绕过单次 30 秒上限或造成除零、超长周期和错误枚举。 */
bool ControlConfig_Valid(const ControlConfig *c)
{
  if (c == NULL) { return false; }
  return c->manual <= 1U && c->collection_mode <= COLLECTION_SCHEDULE &&
    c->start <= 1439U && c->end <= 1439U &&
    (c->collection_mode != COLLECTION_SCHEDULE || c->start != c->end) &&
    c->sample >= 2U && c->sample <= 3600U && c->publish >= 5U && c->publish <= 86400U &&
    PlanValid(&c->brush_plan, true) && PlanValid(&c->actuator_plan, false) &&
    c->cleaning.enabled <= 1U && c->cleaning.interval >= 60U &&
    c->cleaning.interval <= 604800U && c->cleaning.extend >= 1U &&
    c->cleaning.extend <= 30U && c->cleaning.brush >= 1U &&
    c->cleaning.brush <= 30U && c->cleaning.retract >= 1U &&
    c->cleaning.retract <= 30U && c->cleaning.settle <= 600U;
}

/* 使用标准反射 CRC32，记录覆盖不含最后提交字。 */
uint32_t ControlConfig_Crc(const uint8_t *data, uint32_t length)
{
  uint32_t crc = UINT32_MAX, i, bit;
  for (i = 0U; i < length; ++i)
  {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; ++bit)
    { crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320UL : 0U); }
  }
  return ~crc;
}

/* 明确写小端，不依赖结构体填充、主机字节序和编译器布局。 */
static void Put32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
  p[2] = (uint8_t)(value >> 16U); p[3] = (uint8_t)(value >> 24U);
}
/* 明确读取日志的小端整数。 */
static uint32_t Get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
    ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

/* 列出协议配置成员，避免把内存布局当作持久化 ABI。 */
static void ToWords(const ControlConfig *c, uint32_t *w)
{
  w[0]=c->manual; w[1]=c->collection_mode; w[2]=c->start; w[3]=c->end;
  w[4]=c->sample; w[5]=c->publish;
  w[6]=c->brush_plan.type; w[7]=c->brush_plan.start; w[8]=c->brush_plan.interval;
  w[9]=c->brush_plan.duration; w[10]=c->brush_plan.action;
  w[11]=c->actuator_plan.type; w[12]=c->actuator_plan.start; w[13]=c->actuator_plan.interval;
  w[14]=c->actuator_plan.duration; w[15]=c->actuator_plan.action;
  w[16]=c->cleaning.enabled; w[17]=c->cleaning.interval; w[18]=c->cleaning.extend;
  w[19]=c->cleaning.brush; w[20]=c->cleaning.retract; w[21]=c->cleaning.settle;
}
/* 将已校验字序映射回配置，所有保留字由解码入口验证。 */
static void FromWords(ControlConfig *c, const uint32_t *w)
{
  c->manual=w[0]; c->collection_mode=w[1]; c->start=w[2]; c->end=w[3];
  c->sample=w[4]; c->publish=w[5];
  c->brush_plan.type=w[6]; c->brush_plan.start=w[7]; c->brush_plan.interval=w[8];
  c->brush_plan.duration=w[9]; c->brush_plan.action=w[10];
  c->actuator_plan.type=w[11]; c->actuator_plan.start=w[12]; c->actuator_plan.interval=w[13];
  c->actuator_plan.duration=w[14]; c->actuator_plan.action=w[15];
  c->cleaning.enabled=w[16]; c->cleaning.interval=w[17]; c->cleaning.extend=w[18];
  c->cleaning.brush=w[19]; c->cleaning.retract=w[20]; c->cleaning.settle=w[21];
}

/* CRC 放在记录最后，Flash 仅最后提交成功时才出现有效新记录。 */
bool ControlConfig_Encode(const ControlConfig *c, uint32_t sequence, uint8_t *r)
{
  uint32_t w[22], i;
  if (r == NULL || !ControlConfig_Valid(c) || sequence == 0U) { return false; }
  memset(r, 0, CONTROL_CONFIG_RECORD_SIZE);
  Put32(r, CONFIG_MAGIC); Put32(r+4, CONFIG_VERSION); Put32(r+8, sequence);
  ToWords(c, w);
  for (i=0U; i<22U; ++i) { Put32(r+12U+4U*i, w[i]); }
  Put32(r+124, ControlConfig_Crc(r, 124U));
  return true;
}

/* 先查签名、版本、保留字段与 CRC，再验证所有配置语义。 */
bool ControlConfig_Decode(const uint8_t *r, ControlConfig *c, uint32_t *sequence)
{
  uint32_t w[22], i;
  ControlConfig decoded;
  if (r == NULL || c == NULL || Get32(r) != CONFIG_MAGIC ||
      Get32(r+4) != CONFIG_VERSION || Get32(r+8) == 0U ||
      Get32(r+124) != ControlConfig_Crc(r, 124U)) { return false; }
  for (i=100U; i<124U; ++i) { if (r[i] != 0U) { return false; } }
  for (i=0U; i<22U; ++i) { w[i]=Get32(r+12U+4U*i); }
  FromWords(&decoded, w);
  if (!ControlConfig_Valid(&decoded)) { return false; }
  *c=decoded;
  if (sequence != NULL) { *sequence=Get32(r+8); }
  return true;
}
