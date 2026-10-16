/* 文件：board_config.h */
/* 功能：集中定义新水质主控板的引脚、有效电平、串口和保护参数。 */
/*
 * 水质主控板硬件映射。
 * 引脚引用 CubeMX 生成的 main.h，有效电平和保护间隔集中维护。
 * 更换引脚或串口参数时必须同步修改 .ioc，不能只改宏定义。
 */
#ifndef __BOARD_CONFIG_H__
#define __BOARD_CONFIG_H__

#include "main.h"
#include "usart.h"

/* Wi-Fi ESP-12F: PA9 为 STM32 TX，PA10 为 STM32 RX。 */
#define BOARD_WIFI_UART              (&huart1)

/* Air780EP: PD5 为 STM32 TX，PD6 为 STM32 RX，PD4 为 RDY 输入。 */
#define BOARD_4G_UART                (&huart2)
#define BOARD_4G_RDY_PORT            MODEM_RDY_GPIO_Port
#define BOARD_4G_RDY_PIN             MODEM_RDY_Pin

/* RS485: PD8 为 STM32 TX，PD9 为 STM32 RX，外部电路自动处理收发方向。 */
#define BOARD_RS485_UART             (&huart3)

/* 板载状态灯，均为低电平点亮。 */
#define BOARD_RUN_LED_PORT           LED_RUN_GPIO_Port
#define BOARD_RUN_LED_PIN            LED_RUN_Pin
#define BOARD_ERR_LED_PORT           LED_ERR_GPIO_Port
#define BOARD_ERR_LED_PIN            LED_ERR_Pin

/* 三色灯：LED1=红，LED2=黄，LED3=绿，均为低电平点亮。 */
#define BOARD_ALARM_RED_PORT         ALARM_RED_GPIO_Port
#define BOARD_ALARM_RED_PIN          ALARM_RED_Pin
#define BOARD_ALARM_YELLOW_PORT      ALARM_YELLOW_GPIO_Port
#define BOARD_ALARM_YELLOW_PIN       ALARM_YELLOW_Pin
#define BOARD_ALARM_GREEN_PORT       ALARM_GREEN_GPIO_Port
#define BOARD_ALARM_GREEN_PIN        ALARM_GREEN_Pin

/* 毛刷电机，低电平运行。 */
#define BOARD_BRUSH_MOTOR_PORT       BRUSH_MOTOR_GPIO_Port
#define BOARD_BRUSH_MOTOR_PIN        BRUSH_MOTOR_Pin

/* 伸缩杆驱动输入。当前按 IN1=PB6、IN2=PB7。 */
#define BOARD_ACTUATOR_IN1_PORT      ACTUATOR_IN1_GPIO_Port
#define BOARD_ACTUATOR_IN1_PIN       ACTUATOR_IN1_Pin
#define BOARD_ACTUATOR_IN2_PORT      ACTUATOR_IN2_GPIO_Port
#define BOARD_ACTUATOR_IN2_PIN       ACTUATOR_IN2_Pin

/*
 * 伸缩杆方向逻辑。
 * DRV8870 直接逻辑输入：10/01 为两个方向，00 为停止。
 * 电气正转不等于机械伸出；首次实机测试若方向相反，
 * 只需交换下面两组电平，不需要修改状态机。
 */
#define BOARD_ACTUATOR_FORWARD_IN1   GPIO_PIN_SET
#define BOARD_ACTUATOR_FORWARD_IN2   GPIO_PIN_RESET
#define BOARD_ACTUATOR_REVERSE_IN1   GPIO_PIN_RESET
#define BOARD_ACTUATOR_REVERSE_IN2   GPIO_PIN_SET
#define BOARD_ACTUATOR_STOP_IN1      GPIO_PIN_RESET
#define BOARD_ACTUATOR_STOP_IN2      GPIO_PIN_RESET

/* 光耦低电平导通，拉低 P-MOS 栅极供电；高电平关闭传感器电源。 */
#define BOARD_SENSOR1_POWER_PORT     SENSOR_POWER_1_GPIO_Port
#define BOARD_SENSOR1_POWER_PIN      SENSOR_POWER_1_Pin
#define BOARD_SENSOR2_POWER_PORT     SENSOR_POWER_2_GPIO_Port
#define BOARD_SENSOR2_POWER_PIN      SENSOR_POWER_2_Pin
#define BOARD_SENSOR_POWER_ON_LEVEL  GPIO_PIN_RESET
#define BOARD_SENSOR_POWER_OFF_LEVEL GPIO_PIN_SET

/* 预留继电器接口，原理图为低电平吸合。 */
#define BOARD_RELAY1_PORT            RELAY_1_GPIO_Port
#define BOARD_RELAY1_PIN             RELAY_1_Pin
#define BOARD_RELAY2_PORT            RELAY_2_GPIO_Port
#define BOARD_RELAY2_PIN             RELAY_2_Pin
#define BOARD_RELAY_ON_LEVEL         GPIO_PIN_RESET
#define BOARD_RELAY_OFF_LEVEL        GPIO_PIN_SET

/* 波特率与 .ioc 配置保持一致；这些宏本身不会重新配置 UART 硬件。 */
#define BOARD_WIFI_BAUDRATE          115200U
#define BOARD_4G_BAUDRATE            115200U
#define BOARD_RS485_BAUDRATE         9600U

/* 调试阶段的动作最长保护时间，不代表最终清洗时长或机械限位。 */
#define BOARD_MOTION_MAX_MS         30000U  /* 单次毛刷或推杆通电的最长时限，单位 ms。 */
#define BOARD_REVERSAL_GUARD_MS      100U    /* 推杆停机至允许下一方向动作的最短间隔。 */
#define BOARD_RS485_FRAME_GAP_MS       5U    /* 9600 波特率下用于识别应答帧结束的静默时间。 */
#define BOARD_RS485_QUERY_GAP_MS      50U    /* 上次事务结束至下一次请求的最短等待时间。 */

#endif /* __BOARD_CONFIG_H__ */
