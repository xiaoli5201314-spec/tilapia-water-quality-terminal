/* 文件：water_sensor.h */
/* 功能：pH、溶解氧、水温和饱和度传感器读取与数据解码接口。 */
/* 当前部署的秉晟 pH 与溶解氧探头：地址 1/2，9600 8N1。 */
#ifndef __WATER_SENSOR_H__
#define __WATER_SENSOR_H__
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
/* slave_address 是查询地址；enabled 只允许/禁止本地查询，不控制探头供电。 */
typedef struct { uint8_t slave_address; bool enabled; } WaterSensorConfig;
/** 初始化本地配置：索引 0 为 pH 地址 1，索引 1 为溶解氧地址 2；不修改探头内部地址。 */
void WaterSensor_Init(void);
/** 复制 config 到 index 对应的本地配置，index 仅为 0/1；地址须为 1~247，无效参数忽略。 */
void WaterSensor_SetConfig(uint8_t index, const WaterSensorConfig *config);
/** 返回 index 对应的内部只读配置指针，无效索引返回 NULL；内容会随 Init/SetConfig 更新。 */
const WaterSensorConfig *WaterSensor_GetConfig(uint8_t index);
/**
 * @brief 按本地探头配置构造一次 0x03 请求，不发送串口数据。
 * @param index 0 为 pH，1 为溶解氧；该探头必须在本地配置中启用。
 * @param start_register 零起始寄存器地址；register_count 为数量 1~125。
 * @param frame 输出缓冲区；capacity 至少为 8 字节。
 * @param frame_length 必须非空；成功写入 8，失败写入 0。
 * @return HAL_OK 表示构帧成功；HAL_ERROR 表示参数错误或通道未启用。
 */
HAL_StatusTypeDef WaterSensor_BuildReadRequest(uint8_t index,
  uint16_t start_register, uint16_t register_count, uint8_t *frame,
  uint16_t capacity, uint16_t *frame_length);
/**
 * @brief 同步读取 pH，使用索引 0、寄存器 0x0000，读取 1 个寄存器。
 * @param ph 非空输出指针，成功写入 pH 数值；失败保留调用前的值。
 * @return HAL_OK 为成功；HAL_BUSY/HAL_TIMEOUT 为事务状态；其他校验或供电错误为 HAL_ERROR。
 * @note 调用前必须打开通道 1 电源并在业务层等待探头稳定，本函数不自动供电。
 */
HAL_StatusTypeDef WaterSensor_ReadPh(float *ph);
/**
 * @brief 同步读取溶解氧和水温，使用索引 1、寄存器 0x0000，读取 4 个寄存器。
 * @param do_mg_l 非空输出指针，单位 mg/L；water_temp 为非空水温输出指针，单位摄氏度。
 * @return HAL_OK 时同时更新两个输出；失败时两个值均不变，状态含义同 ReadPh。
 * @note 调用前应打开通道 2 电源并等待稳定；当前整数解码方式沿用现有 ESP32 固件。
 */
HAL_StatusTypeDef WaterSensor_ReadOxygen(float *do_mg_l, float *water_temp);
/**
 * @brief 同步读取溶解氧饱和度，使用索引 1、寄存器 0x0004，读取 2 个寄存器。
 * @param saturation 非空输出指针，单位 %；仅成功时更新。
 * @return 状态含义同 ReadPh；通道 2 必须已供电并稳定。
 */
HAL_StatusTypeDef WaterSensor_ReadSaturation(float *saturation);
/**
 * @brief 解码纯数据区，不包含从机地址、功能码、字节数或 CRC。
 * @param payload 两字节大端整数；length 必须为 2；ph 为非空输出指针。
 * @return true 时按原值 / 100 更新 ph；false 时输出不变。不做水质量程或告警判定。
 */
bool WaterSensor_DecodePh(const uint8_t *payload, uint16_t length, float *ph);
/**
 * @brief 解码纯数据区的溶解氧与水温，不检查整帧 CRC。
 * @param payload 八字节：溶氧原值、其小数位数、水温原值、其小数位数，均为大端。
 * @param length 必须为 8；do_mg_l、water_temp 为非空输出指针。
 * @return 两个小数位数都在 0~4 时返回 true 并同时更新；否则 false，两个输出均不变。
 */
bool WaterSensor_DecodeOxygen(const uint8_t *payload, uint16_t length,
                             float *do_mg_l, float *water_temp);
/**
 * @brief 解码纯数据区中的饱和度数值与小数位数。
 * @param payload 四字节大端数据；length 必须为 4；saturation 为非空输出指针，单位 %。
 * @return 参数与小数位数有效时返回 true，否则 false 且保留原输出；不检查量程告警。
 */
bool WaterSensor_DecodeSaturation(const uint8_t *payload, uint16_t length, float *saturation);
#endif
