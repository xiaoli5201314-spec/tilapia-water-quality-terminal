/* 文件：modbus_rtu.c */
/* 功能：实现 Modbus RTU CRC、读寄存器请求和应答帧校验。 */
/* 协议层不依赖 GPIO，不猜测从机量程或异常原因。 */
#include "modbus_rtu.h"
#include <stddef.h>
#include <stdbool.h>

/* 按 Modbus 的初值 0xFFFF 与反射多项式 0xA001 逐字节累积校验值。 */
uint16_t ModbusRtu_Crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFFU;
  uint16_t i;
  uint8_t bit;
  if (data == NULL) { return crc; }
  for (i = 0U; i < length; ++i)
  {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0xA001U) : (uint16_t)(crc >> 1U);
    }
  }
  return crc;
}

/* 构造功能码 0x03 的读保持寄存器请求，不执行串口发送。 */
uint16_t ModbusRtu_BuildReadHoldingRegisters(uint8_t slave_address,
  uint16_t start_register, uint16_t register_count, uint8_t *frame, uint16_t capacity)
{
  uint16_t crc;
  if (frame == NULL || capacity < 8U || slave_address == 0U || slave_address > 247U ||
      register_count == 0U || register_count > 125U ||
      ((uint32_t)start_register + register_count) > 65536U) { return 0U; }
  frame[0] = slave_address;
  frame[1] = 0x03U;
  frame[2] = (uint8_t)(start_register >> 8U);
  frame[3] = (uint8_t)start_register;
  frame[4] = (uint8_t)(register_count >> 8U);
  frame[5] = (uint8_t)register_count;
  /* Modbus 的 CRC 字节顺序与前面的寄存器地址不同，必须低字节先发送。 */
  crc = ModbusRtu_Crc16(frame, 6U);
  frame[6] = (uint8_t)crc;
  frame[7] = (uint8_t)(crc >> 8U);
  return 8U;
}

/* 先检查完整帧长度再访问末尾两个字节，避免越界校验。 */
static bool FrameHasValidCrc(const uint8_t *frame, uint16_t length)
{
  uint16_t received;
  if (frame == NULL || length < 5U || length > 256U) { return false; }
  received = (uint16_t)frame[length - 2U] | ((uint16_t)frame[length - 1U] << 8U);
  return received == ModbusRtu_Crc16(frame, (uint16_t)(length - 2U));
}

/* 校验正常读寄存器应答的 CRC、地址、功能码与字节数。 */
HAL_StatusTypeDef ModbusRtu_ValidateResponse(const uint8_t *frame,
  uint16_t length, uint8_t slave_address, uint8_t function_code, uint8_t *payload_length)
{
  if (payload_length != NULL) { *payload_length = 0U; }
  if (!FrameHasValidCrc(frame, length) || slave_address == 0U || slave_address > 247U ||
      (function_code != 0x03U && function_code != 0x04U)) { return HAL_ERROR; }
  if (frame[0] != slave_address || frame[1] != function_code || frame[2] == 0U ||
      frame[2] > 250U || (frame[2] & 1U) != 0U || (uint16_t)(frame[2] + 5U) != length)
  { return HAL_ERROR; }
  if (payload_length != NULL) { *payload_length = frame[2]; }
  return HAL_OK;
}

/* 只有 CRC 正确、地址匹配的 0x83 帧才报告从机异常；正常帧还需核对请求数量。 */
ModbusResult ModbusRtu_ValidateReadResponse(const uint8_t *frame, uint16_t length,
  uint8_t slave_address, uint16_t expected_count, uint8_t *exception_code)
{
  if (exception_code != NULL) { *exception_code = 0U; }
  if (expected_count == 0U || expected_count > 125U || slave_address == 0U ||
      slave_address > 247U || !FrameHasValidCrc(frame, length) ||
      frame[0] != slave_address) { return MODBUS_RESULT_INVALID; }
  /* 异常应答固定 5 字节，不能按正常数据长度一直等待。 */
  if (frame[1] == 0x83U && length == 5U && frame[2] != 0U)
  {
    if (exception_code != NULL) { *exception_code = frame[2]; }
    return MODBUS_RESULT_EXCEPTION;
  }
  if (ModbusRtu_ValidateResponse(frame, length, slave_address, 0x03U, NULL) != HAL_OK ||
      frame[2] != expected_count * 2U) { return MODBUS_RESULT_INVALID; }
  return MODBUS_RESULT_OK;
}
