/* 文件：test_main.c */
/* 模块：伸缩杆 / 推杆 H 桥（IN1 = PB6, IN2 = PB7 -> DRV8870DDAR -> CN2） */
/* 功能：第 1 步验证 —— 不接推杆，只量 H 桥输出的电压和极性。 */
/*
 * DRV8870 真值表（原理图 SCH_伸缩杆）：
 *   IN1=0 IN2=0  ->  OUT1/OUT2 滑行（高阻）   量 CN2(1-2) = 0V
 *   IN1=1 IN2=0  ->  OUT1=+24V, OUT2=0V      量 CN2(1-2) = +24V
 *   IN1=0 IN2=1  ->  OUT1=0V,   OUT2=+24V    量 CN2(1-2) = -24V
 *   IN1=1 IN2=1  ->  刹车（两个下管导通），本测试不用
 *
 * 测量方法：万用表红笔接 CN2 的 1 脚，黑笔接 CN2 的 2 脚。
 *
 * 时序（循环，每档 10 秒）：
 *   [00] 10s 量 0V  ->  [10] 10s 量 +24V  ->  [00] 10s 量 0V  ->  [01] 10s 量 -24V  ->  循环
 *
 * ⚠️ 本版本用于【空载测电压】，接推杆前请先确认 H 桥极性正确。
 *    确认无误后，把 ALLOW_ACTUATOR_CONNECTED 改成 1 再测机械（时长会压到 1.5 秒）。
 *
 * 灯（RUN = PE2 = 黄灯，ERR = PE3 = 红灯）：
 *   黄灯灭        = 00 停止/滑行
 *   黄灯常亮      = 10 正转
 *   黄灯 1Hz 闪   = 01 反转
 *   红灯每 5 秒闪 = 程序在跑
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ---------------- 引脚（与 main.h / .ioc 一致） ---------------- */
#define IN1_PORT   GPIOB
#define IN1_PIN    GPIO_PIN_6      /* ACTUATOR_IN1 -> DRV8870 pin3 */
#define IN2_PORT   GPIOB
#define IN2_PIN    GPIO_PIN_7      /* ACTUATOR_IN2 -> DRV8870 pin2 */

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

/* 0 = 空载测电压，每档 10 秒（当前版本）
 * 1 = 接推杆测机械，每档压到 1.5 秒（确认 H 桥极性后再改 1） */
#define ALLOW_ACTUATOR_CONNECTED   0

#if (ALLOW_ACTUATOR_CONNECTED != 0)
  #define HOLD_MS        1500U   /* 接机械：小步走，先看方向 */
  #define GAP_MS         3000U   /* 机械档位之间的停顿 */
#else
  #define HOLD_MS       10000U   /* 空载：慢慢量 */
  #define GAP_MS        10000U
#endif

/* 1 = 通过 USART2(PD5/PD6) 打印调试日志；0 = 关闭 */
#define TEST_DEBUG_UART   1U

/* ---------------- H 桥控制 ---------------- */
static void ActuatorWrite(int in1, int in2)
{
  /* 先清零两脚，避免切换瞬间出现 11（刹车）状态 */
  HAL_GPIO_WritePin(IN1_PORT, IN1_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(IN2_PORT, IN2_PIN, GPIO_PIN_RESET);
  if (in1) { HAL_GPIO_WritePin(IN1_PORT, IN1_PIN, GPIO_PIN_SET); }
  if (in2) { HAL_GPIO_WritePin(IN2_PORT, IN2_PIN, GPIO_PIN_SET); }
}

/* ---------------- 调试日志 ---------------- */
#if (TEST_DEBUG_UART != 0U)
static void LogStr(const char *s)
{
  if (s == NULL) { return; }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}
static void LogLine(const char *s) { LogStr(s); LogStr("\r\n"); }
static void LogPhase(const char *name, int in1, int in2, const char *expect)
{
  char buf[144];
  int n = snprintf(buf, sizeof(buf),
    "[%s] IN1(PB6)=%d IN2(PB7)=%d  -> CN2(1-2) should be %s\r\n",
    name,
    (int)HAL_GPIO_ReadPin(IN1_PORT, IN1_PIN),
    (int)HAL_GPIO_ReadPin(IN2_PORT, IN2_PIN),
    expect);
  if (n > 0) { LogStr(buf); }
}
#else
static void LogStr(const char *s) { (void)s; }
static void LogLine(const char *s) { (void)s; }
static void LogPhase(const char *n, int a, int b, const char *e)
{ (void)n; (void)a; (void)b; (void)e; }
#endif

/* 保持某档 HOLD_MS，期间红灯每 5 秒闪一下表示活着；running_pattern 控制黄灯指示。 */
static void HoldPhase(int in1, int in2, uint32_t total_ms, int lamp_mode)
{
  uint32_t elapsed = 0U;
  ActuatorWrite(in1, in2);

  while (elapsed < total_ms)
  {
    /* 红灯：每 5 秒闪 100ms，表示程序在跑 */
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN,
      ((elapsed % 5000U) < 100U) ? GPIO_PIN_RESET : GPIO_PIN_SET);

    /* 黄灯：0=灭, 1=常亮, 2=1Hz 闪 */
    if (lamp_mode == 1)
    { HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_RESET); }
    else if (lamp_mode == 2)
    { HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN,
        ((elapsed % 1000U) < 500U) ? GPIO_PIN_RESET : GPIO_PIN_SET); }
    else
    { HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_SET); }

    HAL_Delay(20U);
    elapsed += 20U;
  }
  /* 档位结束先回到停止态 */
  ActuatorWrite(0, 0);
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
  /* 出错先把 H 桥置成 00（滑行），安全第一 */
  ActuatorWrite(0, 0);
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

  LogStr("\r\n\r\n=== XIAOLI ACTUATOR (DRV8870) TEST ===\r\n");
#if (ALLOW_ACTUATOR_CONNECTED != 0)
  LogLine("mode: WITH actuator  (each phase 1.5s)");
#else
  LogLine("mode: NO actuator    (each phase 10s, measure only)");
#endif
  LogLine("measure CN2: red probe = pin1, black probe = pin2");

  /* 关键：先把 H 桥置 00，防止从 GPIO 初始化到这里的间隙里意外动作 */
  ActuatorWrite(0, 0);

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

  LogLine("init done, cycling...");

  for (;;)
  {
    /* 档 1：00 滑行 -> 0V */
    LogPhase("00  ", 0, 0, "0V");
    HoldPhase(0, 0, HOLD_MS, 0);

    /* 档 2：10 正转 -> +24V */
    LogPhase("10  ", 1, 0, "+24V  (pin1 higher)");
    HoldPhase(1, 0, HOLD_MS, 1);

    /* 档 3：00 滑行 -> 0V */
    LogPhase("00  ", 0, 0, "0V");
    HoldPhase(0, 0, GAP_MS, 0);

    /* 档 4：01 反转 -> -24V */
    LogPhase("01  ", 0, 1, "-24V  (pin2 higher)");
    HoldPhase(0, 1, HOLD_MS, 2);

    /* 回到停止态，循环 */
    ActuatorWrite(0, 0);
    LogLine("");
  }
}
