/* 文件：water_sensor.c */
/* 功能：实现 pH、溶解氧、水温和饱和度的 Modbus 读取与换算。 */
/* 寄存器与换算沿用现有 ESP32 固件，并核对秉晟两份传感器说明书。 */
#include "water_sensor.h"
#include "modbus_rtu.h"
#include "rs485.h"
#include "sensor_power.h"
#include <stddef.h>
#define WATER_SENSOR_COUNT 2U          /* 索引 0=pH，索引 1=溶解氧。 */
#define WATER_SENSOR_TIMEOUT_MS 600U   /* 单次请求的发送与接收超时，单位 ms。 */
static WaterSensorConfig sensor_config[WATER_SENSOR_COUNT];
/* 探头寄存器高字节先到，这里显式组合，避免依赖 MCU 内存字节序。 */
static uint16_t ReadBe16(const uint8_t *bytes)
{
  return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}
/* 四字节表示原始整数和小数位数，比例只接受当前协议使用的 0~4 位。 */
static bool DecodeDecimal(const uint8_t *bytes, float *value)
{
  static const float factors[5] = {1.0F, 10.0F, 100.0F, 1000.0F, 10000.0F};
  uint16_t digits = ReadBe16(bytes + 2U);
  if (digits > 4U) { return false; }
  *value = (float)ReadBe16(bytes) / factors[digits];
  return true;
}

/* 初始化本地配置：索引 0 为 pH 地址 1，索引 1 为溶解氧地址 2；不修改探头内部地址。 */
void WaterSensor_Init(void)
{
  sensor_config[0].slave_address = 0x01U;
  sensor_config[0].enabled = true;
  sensor_config[1].slave_address = 0x02U;
  sensor_config[1].enabled = true;
}
/* 复制 config 到 index 对应的本地配置，index 仅为 0/1；地址须为 1~247，无效参数忽略。 */
void WaterSensor_SetConfig(uint8_t index, const WaterSensorConfig *config)
{
  if (index < WATER_SENSOR_COUNT && config != NULL &&
      config->slave_address >= 1U && config->slave_address <= 247U)
  { sensor_config[index] = *config; }
}
/* 返回 index 对应的内部只读配置指针，无效索引返回 NULL；内容会随 Init/SetConfig 更新。 */
const WaterSensorConfig *WaterSensor_GetConfig(uint8_t index)
{
  return index < WATER_SENSOR_COUNT ? &sensor_config[index] : NULL;
}
/* 按本地探头配置构造一次 0x03 请求，不发送串口数据。 */
HAL_StatusTypeDef WaterSensor_BuildReadRequest(uint8_t index,
  uint16_t start_register, uint16_t register_count, uint8_t *frame,
  uint16_t capacity, uint16_t *frame_length)
{
  uint16_t length;
  if (frame_length != NULL) { *frame_length = 0U; }
  if (index >= WATER_SENSOR_COUNT || frame_length == NULL ||
      !sensor_config[index].enabled) { return HAL_ERROR; }
  length = ModbusRtu_BuildReadHoldingRegisters(sensor_config[index].slave_address,
                                             start_register, register_count, frame, capacity);
  if (length == 0U) { return HAL_ERROR; }
  *frame_length = length;
  return HAL_OK;
}

/* 依次检查供电、构帧、事务和应答一致性；任何环节失败均不交付测量数据。 */
static HAL_StatusTypeDef ReadRegisters(uint8_t index, uint16_t start, uint16_t count,
                                      uint8_t *response, uint16_t capacity)
{
  uint8_t request[8];
  uint16_t request_length;
  uint16_t response_length;
  HAL_StatusTypeDef status;
  if (index >= WATER_SENSOR_COUNT || !SensorPower_IsOn((SensorPowerChannel)index))
  { return HAL_ERROR; }
  status = WaterSensor_BuildReadRequest(index, start, count, request, sizeof(request), &request_length);
  if (status != HAL_OK) { return status; }
  status = Rs485_Transfer(request, request_length, response, capacity, &response_length,
                          WATER_SENSOR_TIMEOUT_MS);
  if (status != HAL_OK) { return status; }
  /* 只有请求地址、数量及 CRC 全部一致的数据才能进入数值解码。 */
  return ModbusRtu_ValidateReadResponse(response, response_length,
    sensor_config[index].slave_address, count, NULL) == MODBUS_RESULT_OK ? HAL_OK : HAL_ERROR;
}

/* 解码纯数据区，不包含从机地址、功能码、字节数或 CRC。 */
bool WaterSensor_DecodePh(const uint8_t *payload, uint16_t length, float *ph)
{
  if (payload == NULL || length != 2U || ph == NULL) { return false; }
  *ph = (float)ReadBe16(payload) / 100.0F;
  return true;
}
/* 先把两个量解码到临时变量，全部有效才同时写回，避免半新半旧数据。 */
bool WaterSensor_DecodeOxygen(const uint8_t *payload, uint16_t length,
                             float *do_mg_l, float *water_temp)
{
  float oxygen;
  float temp;
  if (payload == NULL || length != 8U || do_mg_l == NULL || water_temp == NULL ||
      !DecodeDecimal(payload, &oxygen) || !DecodeDecimal(payload + 4U, &temp)) { return false; }
  *do_mg_l = oxygen;
  *water_temp = temp;
  return true;
}
/* 解码纯数据区中的饱和度数值与小数位数。 */
bool WaterSensor_DecodeSaturation(const uint8_t *payload, uint16_t length, float *saturation)
{
  if (payload == NULL || length != 4U || saturation == NULL) { return false; }
  return DecodeDecimal(payload, saturation);
}

/* 同步读取 pH，使用索引 0、寄存器 0x0000，读取 1 个寄存器。 */
HAL_StatusTypeDef WaterSensor_ReadPh(float *ph)
{
  uint8_t response[16];
  HAL_StatusTypeDef status;
  if (ph == NULL) { return HAL_ERROR; }
  status = ReadRegisters(0U, 0x0000U, 1U, response, sizeof(response));
  if (status != HAL_OK) { return status; }
  return WaterSensor_DecodePh(response + 3U, 2U, ph) ? HAL_OK : HAL_ERROR;
}
/* 同步读取溶解氧和水温，使用索引 1、寄存器 0x0000，读取 4 个寄存器。 */
HAL_StatusTypeDef WaterSensor_ReadOxygen(float *do_mg_l, float *water_temp)
{
  uint8_t response[16];
  HAL_StatusTypeDef status;
  if (do_mg_l == NULL || water_temp == NULL) { return HAL_ERROR; }
  status = ReadRegisters(1U, 0x0000U, 4U, response, sizeof(response));
  if (status != HAL_OK) { return status; }
  return WaterSensor_DecodeOxygen(response + 3U, 8U, do_mg_l, water_temp) ? HAL_OK : HAL_ERROR;
}
/* 同步读取溶解氧饱和度，使用索引 1、寄存器 0x0004，读取 2 个寄存器。 */
HAL_StatusTypeDef WaterSensor_ReadSaturation(float *saturation)
{
  uint8_t response[16];
  HAL_StatusTypeDef status;
  if (saturation == NULL) { return HAL_ERROR; }
  status = ReadRegisters(1U, 0x0004U, 2U, response, sizeof(response));
  if (status != HAL_OK) { return status; }
  return WaterSensor_DecodeSaturation(response + 3U, 4U, saturation) ? HAL_OK : HAL_ERROR;
}
