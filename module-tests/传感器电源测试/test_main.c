/* 文件：test_main.c */
/* 模块：传感器 12V 电源（PD11 -> CN3.1 / PD10 -> CN4.1） */
/* 功能：上电即让两个引脚输出【低电平】并一直保持，供万用表测量。 */
/*
 * 本版本很简单：上电 -> PD11/PD10 输出低(0V) -> 一直保持，不切换。
 *
 * 测量两个点：
 *   测量点 A：MCU 引脚 PD11 / PD10   -> 应读到 0V
 *   测量点 B：CN3.1 / CN4.1 对 GND   -> 看这里读出多少，用来定"低电平到底是供电还是断电"
 *
 * 灯（RUN = PE2 = 黄灯，ERR = PE3 = 红灯）：
 *   黄灯常亮 = PD11(引脚1) 当前是低电平
 *   红灯常亮 = PD10(引脚2) 当前是低电平
 *   两灯都亮 = 两个引脚都是低（本版本正常状态）
 *
 * 想测高电平版本，把下面的 OUTPUT_LEVEL 改成 1 重新编译。
 *   OUTPUT_LEVEL = 0 -> 输出低电平 0V
 *   OUTPUT_LEVEL = 1 -> 输出高电平 3.3V
 *
 * 日志（可选）：USB-TTL 接 PD5(TX)/PD6(RX)/GND，115200 8N1。
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ==================== 本版本要输出的电平 ==================== */
/* 0 = 输出低电平 0V（当前版本）   1 = 输出高电平 3.3V */
#define OUTPUT_LEVEL   0

/* 1 = 通过 USART2(PD5/PD6) 打印调试日志；0 = 关闭 */
#define TEST_DEBUG_UART   1U

/* ---------------- 引脚（与 main.h / .ioc 一致） ---------------- */
#define S1_PORT   GPIOD
#define S1_PIN    GPIO_PIN_11     /* RS1_12V -> CN3.1 */
#define S2_PORT   GPIOD
#define S2_PIN    GPIO_PIN_10     /* RS2_12V -> CN4.1 */

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

/* ---------------- 目标电平 ---------------- */
#if (OUTPUT_LEVEL == 0)
  #define TARGET_LEVEL   GPIO_PIN_RESET
  #define TARGET_TEXT    "LOW (0V)"
#else
  #define TARGET_LEVEL   GPIO_PIN_SET
  #define TARGET_TEXT    "HIGH (3.3V)"
#endif

static void SensorWrite(GPIO_PinState level)
{
  HAL_GPIO_WritePin(S1_PORT, S1_PIN, level);
  HAL_GPIO_WritePin(S2_PORT, S2_PIN, level);
}

/* ---------------- 调试日志 ---------------- */
#if (TEST_DEBUG_UART != 0U)
static void LogStr(const char *s)
{
  if (s == NULL) { return; }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}
static void LogLine(const char *s) { LogStr(s); LogStr("\r\n"); }
static void LogState(void)
{
  char buf[160];
  int n = snprintf(buf, sizeof(buf),
    "PD11=%d PD10=%d | expect pin = %s | measure CN3.1 / CN4.1 now\r\n",
    (int)HAL_GPIO_ReadPin(S1_PORT, S1_PIN),
    (int)HAL_GPIO_ReadPin(S2_PORT, S2_PIN),
    TARGET_TEXT);
  if (n > 0) { LogStr(buf); }
}
#else
static void LogStr(const char *s) { (void)s; }
static void LogLine(const char *s) { (void)s; }
static void LogState(void) { }
#endif

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
  SensorWrite(GPIO_PIN_RESET);   /* 出错也保持低电平，便于排障 */
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
  uint32_t log_at;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART2_UART_Init();   /* 仅用于日志 */

  LogStr("\r\n\r\n=== XIAOLI SENSOR 12V POWER (FIXED LEVEL) ===\r\n");
  LogStr("target output level: ");
  LogLine(TARGET_TEXT);
  LogLine("MEASURE: (A) MCU pin PD11/PD10   (B) CN3.1 / CN4.1 to GND");

  /* 关键：尽快把引脚设成目标电平 */
  SensorWrite(TARGET_LEVEL);

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

  LogLine("init done, holding level.");
  LogState();

  log_at = HAL_GetTick();

  for (;;)
  {
    /* 引脚电平由 ReadPin 直接反映到灯上：
     *   黄灯亮 = PD11 是低电平（0V）
     *   红灯亮 = PD10 是低电平（0V）
     * 两灯都亮 = 两个引脚都是低（本版本 OUTPUT_LEVEL=0 时的正常现象） */
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN,
      (HAL_GPIO_ReadPin(S1_PORT, S1_PIN) == GPIO_PIN_RESET) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN,
      (HAL_GPIO_ReadPin(S2_PORT, S2_PIN) == GPIO_PIN_RESET) ? GPIO_PIN_RESET : GPIO_PIN_SET);

    /* 每秒重新写一次目标电平，防止被干扰改写 */
    SensorWrite(TARGET_LEVEL);

    /* 每 3 秒打一次日志 */
    if ((HAL_GetTick() - log_at) >= 3000U)
    {
      log_at = HAL_GetTick();
      LogState();
    }
    HAL_Delay(100U);
  }
}
