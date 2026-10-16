/* 文件：config_flash.c */
/* 功能：末扇区追加配置日志，先写数据后提交 CRC，不自动擦除任何已有数据。 */
#include "control_config.h"
#include "stm32f4xx_hal.h"
#include "actuator.h"
#include "brush_motor.h"
#include "cleaning_controller.h"
#include <string.h>
#include <stddef.h>
static uint32_t sequence, next_offset;
static ControlConfig saved;
static bool have_saved;
#ifdef CONTROL_HOST_TEST
static uint8_t storage[CONTROL_CONFIG_FLASH_SIZE];
static bool storage_ready;
/* 主机独立模拟掉电后的 Flash，初始化为擦除态。 */
void ControlConfig_ResetStorage(void)
{
  memset(storage,0xFF,sizeof(storage)); storage_ready=true;
  sequence=0U; next_offset=0U; have_saved=false;
}
#endif
/* 只读保留区，主机与实机使用同一记录扫描算法。 */
static const uint8_t *Record(uint32_t offset)
{
#ifdef CONTROL_HOST_TEST
  if (!storage_ready) { ControlConfig_ResetStorage(); }
  return storage+offset;
#else
  return (const uint8_t *)(CONTROL_CONFIG_FLASH_BASE+offset);
#endif
}
/* 全记录擦除态才允许使用，不覆盖未知或损坏的旧内容。 */
static bool Blank(const uint8_t *record)
{
  uint32_t i;
  for (i=0U;i<CONTROL_CONFIG_RECORD_SIZE;++i) { if (record[i]!=0xFFU) { return false; } }
  return true;
}
/* 扫描全部槽位，CRC 损坏的半写记录跳过，并选取最大有效序号。 */
bool ControlConfig_Load(ControlConfig *config)
{
  uint32_t offset, found_sequence;
  ControlConfig decoded;
  sequence=0U; next_offset=CONTROL_CONFIG_FLASH_SIZE; have_saved=false;
  if (config==NULL) { return false; }
  ControlConfig_Default(config);
  for (offset=0U;offset<CONTROL_CONFIG_FLASH_SIZE;offset+=CONTROL_CONFIG_RECORD_SIZE)
  {
    const uint8_t *r=Record(offset);
    if (Blank(r) && next_offset==CONTROL_CONFIG_FLASH_SIZE) { next_offset=offset; }
    if (ControlConfig_Decode(r,&decoded,&found_sequence) && found_sequence>sequence)
    { sequence=found_sequence; *config=decoded; have_saved=true; }
  }
  saved=*config;
  return have_saved;
}
/* 单词写入后读回，最后一个 CRC 字才使新记录生效。 */
static bool Program(const uint8_t *record, uint32_t offset)
{
#ifdef CONTROL_HOST_TEST
  memcpy(storage+offset,record,CONTROL_CONFIG_RECORD_SIZE);
#else
  uint32_t i;
  uint32_t cache_enabled=FLASH->ACR&FLASH_ACR_DCEN;
  bool ok=true;
  if (HAL_FLASH_Unlock()!=HAL_OK) { return false; }
  /* 先扫描过的擦除字可能仍在 ART 数据缓存中，禁用并复位后再编程/校验。 */
  __HAL_FLASH_DATA_CACHE_DISABLE();
  __HAL_FLASH_DATA_CACHE_RESET();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP|FLASH_FLAG_OPERR|FLASH_FLAG_WRPERR|
    FLASH_FLAG_PGAERR|FLASH_FLAG_PGPERR|FLASH_FLAG_PGSERR);
  for (i=0U;i<CONTROL_CONFIG_RECORD_SIZE;i+=4U)
  {
    uint32_t word=(uint32_t)record[i] | ((uint32_t)record[i+1U]<<8U) |
      ((uint32_t)record[i+2U]<<16U) | ((uint32_t)record[i+3U]<<24U);
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,CONTROL_CONFIG_FLASH_BASE+offset+i,word)!=HAL_OK)
    { ok=false; break; }
  }
  (void)HAL_FLASH_Lock();
  __HAL_FLASH_DATA_CACHE_RESET();
  if (cache_enabled!=0U) { __HAL_FLASH_DATA_CACHE_ENABLE(); }
  if (!ok) { return false; }
#endif
  return memcmp(Record(offset),record,CONTROL_CONFIG_RECORD_SIZE)==0;
}
/* 仅实际配置变更时写；动作期间与日志满时拒绝，不擦除已有数据。 */
bool ControlConfig_Save(const ControlConfig *config)
{
  uint8_t record[CONTROL_CONFIG_RECORD_SIZE];
  uint32_t offset;
  if (!ControlConfig_Valid(config)) { return false; }
  if (memcmp(config,&saved,sizeof(saved))==0) { return true; }
  if (Actuator_IsBusy() || BrushMotor_IsRunning() || CleaningController_IsBusy() ||
      sequence==UINT32_MAX || next_offset>=CONTROL_CONFIG_FLASH_SIZE ||
      !ControlConfig_Encode(config,sequence+1U,record)) { return false; }
  offset=next_offset;
  next_offset=CONTROL_CONFIG_FLASH_SIZE;
  /* 占用槽即使写失败也不重写，下一次从新擦除槽开始。 */
  {
    uint32_t candidate;
    for (candidate=offset+CONTROL_CONFIG_RECORD_SIZE;candidate<CONTROL_CONFIG_FLASH_SIZE;
         candidate+=CONTROL_CONFIG_RECORD_SIZE)
    { if (Blank(Record(candidate))) { next_offset=candidate; break; } }
  }
  if (!Program(record,offset)) { return false; }
  ++sequence; saved=*config; have_saved=true;
  return true;
}
