/* 文件：test_main.c */
/* 模块：毛刷电机（MT = PB8，经光耦 EL357N 驱动 N 沟道 IRLR120N 低边开关） */
/* 功能：上电后先停 10 秒，再转 10 秒，循环往复，便于万用表反复测量。 */
/*
 * 有效电平（依据原理图 SCH_马达驱动）：
 *   PB8 = 低(0V) -> 光耦导通 -> 栅极被抬到约 2.5V -> IRLR120N 导通 -> CN1 两端 24V -> 电机转
 *   PB8 = 高(3.3V)-> 光耦不通 -> 栅极被 R5 10k 下拉到地 -> 管子截止 -> CN1 两端 0V   -> 电机停
 *
 * 测量点：CN1 的 1、2 两端（马达驱动图页那个 2 芯座）
 *   引脚 1 = P24V（常接 24V）
 *   引脚 2 = 开关漏极（导通时被拉到 0V）
 *   所以量 1-2 之间：0V = 停，约 24V = 转
 *
 * 时序（循环）：
 *   上电 -> [停 10 秒，量到 0V] -> [转 10 秒，量到 24V] -> [停 10 秒] -> [转 10 秒] -> ...
 *
 * 灯：
 *   RUN = PE2 = 黄灯：灭 = 电机停，亮 = 电机转
 *   ERR = PE3 = 红灯：每 5 秒闪一下，表示程序在跑
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ---------------- 引脚（与 main.h / .ioc 一致） ---------------- */
#define MOTOR_PORT     GPIOB
#define MOTOR_PIN      GPIO_PIN_8    /* MT -> CN1 */

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

#define MOTOR_RUN_LEVEL   GPIO_PIN_RESET   /* 低电平转 */
#define MOTOR_STOP_LEVEL  GPIO_PIN_SET     /* 高电平停 */

#define ON_SECONDS     10U   /* 转动时长 */
#define OFF_SECONDS    10U   /* 停止时长 */

/* 1 = 通过 USART2(PD5/PD6) 打印调试日志；0 = 关闭 */
#define TEST_DEBUG_UART   1U

/* ---------------- 电机控制 ---------------- */
static void MotorRun(void)
{
  HAL_GPIO_WritePin(MOTOR_PORT, MOTOR_PIN, MOTOR_RUN_LEVEL);
}
static void MotorStop(void)
{
  HAL_GPIO_WritePin(MOTOR_PORT, MOTOR_PIN, MOTOR_STOP_LEVEL);
}

/* ---------------- 调试日志 ---------------- */
#if (TEST_DEBUG_UART != 0U)
static void LogStr(const char *s)
{
  if (s == NULL) { return; }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}
static void LogLine(const char *s) { LogStr(s); LogStr("\r\n"); }
static void LogPhase(const char *phase, uint32_t seconds, const char *expect)
{
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "[%s] PB8=%d  hold %lus  -> CN1(1-2) should be %s\r\n",
    phase,
    (int)HAL_GPIO_ReadPin(MOTOR_PORT, MOTOR_PIN),
    (unsigned long)seconds, expect);
  if (n > 0) { LogStr(buf); }
}
#else
static void LogStr(const char *s) { (void)s; }
static void LogLine(const char *s) { (void)s; }
static void LogPhase(const char *p, uint32_t s, const char *e)
{ (void)p; (void)s; (void)e; }
#endif

/* 在 hold 期间维持给定状态，并每 5 秒闪一下 ERR 表示程序活着。 */
static void HoldState(int running, uint32_t total_ms)
{
  uint32_t elapsed = 0U;
  if (running) { MotorRun(); } else { MotorStop(); }
  HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, running ? GPIO_PIN_RESET : GPIO_PIN_SET);

  while (elapsed < total_ms)
  {
    /* 每 5 秒闪一下 ERR（100ms） */
    if ((elapsed % 5000U) < 100U)
    { HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_RESET); }
    else
    { HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_SET); }

    HAL_Delay(50U);
    elapsed += 50U;
  }
  HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_SET);
}

/* ---------------- 最小系统支撑 ---------------- */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 8U;
  osc.PLL.PLLN = 336U;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 7U;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) { Error_Handler(); }

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV4;
  clk.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) { Error_Handler(); }
}

void Error_Handler(void)
{
  MotorStop();               /* 出错先把电机停掉，安全第一 */
  __disable_irq();
  for (;;)
  {
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_SET);
    HAL_Delay(100U);
  }
}

/* ---------------- 主程序 ---------------- */
int main(void)
{
  uint8_t i;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART2_UART_Init();   /* 仅用于日志 */

  LogStr("\r\n\r\n=== XIAOLI BRUSH MOTOR TEST ===\r\n");
  LogLine("measure CN1 pin1-pin2:  0V = stopped, ~24V = running");

  /* 关键：先把电机停稳，再进入循环。防止从 GPIO 初始化到这里的间隙里电机乱动。 */
  MotorStop();

  /* 开机提示：两灯同闪 3 次 */
  for (i = 0U; i < 3U; ++i)
  {
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_RESET);
    HAL_Delay(150U);
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_SET);
    HAL_Delay(150U);
  }

  LogLine("init done, starting cycle: STOP 10s -> RUN 10s -> ...");

  for (;;)
  {
    /* 第一段：停 10 秒，万用表量 CN1 应为 0V */
    LogPhase("STOP", OFF_SECONDS, "0V");
    HoldState(0, OFF_SECONDS * 1000U);
    LogPhase("STOP done", 0U, "was 0V");

    /* 第二段：转 10 秒，万用表量 CN1 应为 24V */
    LogPhase("RUN ", ON_SECONDS, "~24V");
    HoldState(1, ON_SECONDS * 1000U);
    LogPhase("RUN done", 0U, "was ~24V");

    /* 回到停止态再循环 */
    MotorStop();
    LogLine("");
  }
}
