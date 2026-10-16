/* 文件：modbus_rtu.h */
/* 功能：Modbus RTU 读寄存器组帧、CRC 和应答校验接口。 */
/* Modbus RTU 读保持寄存器，CRC 低字节在前，寄存器高字节在前。 */
#ifndef __MODBUS_RTU_H__
#define __MODBUS_RTU_H__
#include "stm32f4xx_hal.h"
#include <stdint.h>
/* INVALID 为帧不匹配或校验失败；EXCEPTION 为校验有效的从机异常应答。 */
typedef enum { MODBUS_RESULT_OK = 0, MODBUS_RESULT_INVALID,
               MODBUS_RESULT_EXCEPTION } ModbusResult;
/**
 * @brief 计算 Modbus CRC16，返回的数值发送时需低字节在前。
 * @param data 原始字节数组；NULL 时返回初始值 0xFFFF，不代表有效校验。
 * @param length 参与计算的字节数，不包含帧末尾已有的 CRC。
 * @return 16 位 CRC 数值；空数据长度也返回初始值。
 */
uint16_t ModbusRtu_Crc16(const uint8_t *data, uint16_t length);
/**
 * @brief 构造功能码 0x03 的读保持寄存器请求，不执行串口发送。
 * @param slave_address 从机地址 1~247，不接受广播地址。
 * @param start_register 起始寄存器协议地址，按零起始地址传入。
 * @param register_count 连续寄存器数量 1~125，末地址不得越过 0xFFFF。
 * @param frame 输出缓冲区；capacity 为其字节容量，至少为 8。
 * @return 成功返回帧长 8；参数错误返回 0，不生成请求帧。
 */
uint16_t ModbusRtu_BuildReadHoldingRegisters(uint8_t slave_address,
  uint16_t start_register, uint16_t register_count, uint8_t *frame, uint16_t capacity);
/**
 * @brief 校验正常读寄存器应答的 CRC、地址、功能码与字节数。
 * @param frame 完整应答帧；length 包含地址、功能码和 CRC，单位字节。
 * @param slave_address 期望从机地址 1~247。
 * @param function_code 只支持 0x03、0x04；异常应答不在此接口中判为成功。
 * @param payload_length 可为 NULL；非空时成功返回数据区字节数，失败置 0。
 * @return 通过返回 HAL_OK，否则 HAL_ERROR；不核对具体请求的寄存器数量。
 */
HAL_StatusTypeDef ModbusRtu_ValidateResponse(const uint8_t *frame,
  uint16_t length, uint8_t slave_address, uint8_t function_code, uint8_t *payload_length);
/**
 * @brief 校验 0x03 应答是否匹配本次读取数量，并区分正常帧和从机异常帧。
 * @param frame 完整应答帧；length 为含 CRC 的总字节数。
 * @param slave_address 期望从机地址；expected_count 为期望寄存器数量 1~125。
 * @param exception_code 可为 NULL；异常帧时输出从机异常码，其他情况置 0。
 * @return MODBUS_RESULT_OK、MODBUS_RESULT_EXCEPTION 或 MODBUS_RESULT_INVALID。
 */
ModbusResult ModbusRtu_ValidateReadResponse(const uint8_t *frame, uint16_t length,
  uint8_t slave_address, uint16_t expected_count, uint8_t *exception_code);
#endif
