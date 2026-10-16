/* 文件：test_main.c */
/* 模块：端到端 + 服务器侧诊断（不需要 USB-TTL，全部信息通过 MQTT 上报） */
/*
 * 设计目标：板子接了两个探头，但你没有 USB-TTL，所以本程序把
 *           【每一步的结果】全部通过 MQTT 发到服务器，
 *           你只要在服务器上订阅，就能看到：
 *             - 板子还在不在（心跳）
 *             - 传感器读到没有、读到了什么、原始寄存器值是多少
 *             - 失败原因（没收到字节 / CRC 错 / 地址错 / 超量程）
 *             - 发布成功和失败各多少次
 *
 * ── 发布的主题（都在你的 127.0.0.1 上）────────────────────
 *
 *   1) 水质数据（和正式工程一致）
 *      fish/device/<UID>/water/latest
 *      {"ph":7.12,"doMgL":8.68,"waterTemp":22.52,"doSaturation":96.50}
 *
 *   2) 诊断信息（本程序新增，用来代替串口日志）
 *      fish/device/<UID>/water/diag
 *      {"id":"000000000000000000000001","up":12,"cycle":1,
 *       "wifi":1,"mqtt":1,"ph":1,"do":1,"sat":1,
 *       "rawPh":712,"rawDo":868,"rawTemp":2252,"rawSat":9650,
 *       "ph":7.12,"doMgL":8.68,"waterTemp":22.52,"doSaturation":96.50,
 *       "pubOk":1,"pubFail":0}
 *
 *      字段含义：
 *        up      开机后秒数
 *        cycle   第几轮
 *        wifi    1 = 连上热点
 *        mqtt    1 = MQTT 已连
 *        ph/do/sat  1 = 该项读取成功（含 CRC 和地址校验）
 *        rawX    读到的原始寄存器值（用来判断换算是都正确）
 *        pubOk / pubFail  发布成功/失败累计次数
 *
 *      注意：诊断每轮都会发（不管成功失败），而且用 retain=1，
 *           所以你随时去服务器订阅，都能看到【最后一轮的状态】。
 *
 *   3) 心跳
 *      fish/device/<UID>/water/alive
 *      {"id":"...","up":12,"wifi":1,"mqtt":1}
 *
 * ── 服务器上怎么看（复制这三条）─────────────────────────────
 *
 *   # 看诊断（最重要，随时都有 retain 的最后一条）
 *   timeout 5 mosquitto_sub -h localhost -u YOUR_MQTT_USER -P 'YOUR_MQTT_PASSWORD' \
 *     -t 'fish/device/+/water/diag' -v -C 1
 *
 *   # 看水质数据
 *   timeout 5 mosquitto_sub -h localhost -u YOUR_MQTT_USER -P 'YOUR_MQTT_PASSWORD' \
 *     -t 'fish/device/+/water/latest' -v -C 1
 *
 *   # 实时看全部（30 秒）
 *   timeout 30 mosquitto_sub -h localhost -u YOUR_MQTT_USER -P 'YOUR_MQTT_PASSWORD' \
 *     -t 'fish/device/#' -v
 *
 * ── 灯（只有两个灯，没有串口）───────────────────────────────
 *   红灯 每 5 秒闪一次 = 程序在循环
 *   黄灯 常亮          = 正在发布
 *   两灯同闪 3 次      = 开机
 *   红灯 常亮不动      = 网络彻底失败（连不上热点或 MQTT）
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ==================== 现场参数 ==================== */
#define WIFI_SSID            "YOUR_WIFI_SSID"
#define WIFI_PASSWORD        "YOUR_WIFI_PASSWORD"

#define MQTT_HOST            "127.0.0.1"
#define MQTT_PORT            1883U
#define MQTT_USER            "YOUR_MQTT_USER"
#define MQTT_PASSWORD        "YOUR_MQTT_PASSWORD"

/* 每多少秒一轮（测试用 10 秒） */
#define CYCLE_SECONDS        10U
/* 探头预热 */
#define WARMUP_SECONDS       10U
/* Modbus 从机地址 */
#define PH_ADDR              0x01U
#define DO_ADDR              0x02U

/* ---------------- 引脚 ---------------- */
#define S1_PORT   GPIOD
#define S1_PIN    GPIO_PIN_11     /* RS1_12V -> CN3.1 (pH) */
#define S2_PORT   GPIOD
#define S2_PIN    GPIO_PIN_10     /* RS2_12V -> CN4.1 (DO) */

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

/* ==================== 灯 ==================== */
static void LedRun(int on)
{ HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }
static void LedErr(int on)
{ HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }

/* ============ 中断接收（USART1=WiFi, USART3=RS485） ============ */
#define RX_CAP 512U

static char     wifi_buf[RX_CAP];
static volatile uint16_t wifi_head;
static uint8_t  wifi_byte;

static uint8_t  rs_buf[RX_CAP];
static volatile uint16_t rs_head;
static uint8_t  rs_byte;

static void WifiRxClear(void)
{ __disable_irq(); wifi_head = 0U; wifi_buf[0] = '\0'; __enable_irq(); }
static uint16_t WifiRxLen(void)
{ uint16_t n; __disable_irq(); n = wifi_head; __enable_irq(); return n; }
static void RsRxClear(void)
{ __disable_irq(); rs_head = 0U; __enable_irq(); }
static uint16_t RsRxLen(void)
{ uint16_t n; __disable_irq(); n = rs_head; __enable_irq(); return n; }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    if (wifi_head < (RX_CAP - 1U))
    { wifi_buf[wifi_head] = (char)wifi_byte; wifi_head++; wifi_buf[wifi_head] = '\0'; }
    (void)HAL_UART_Receive_IT(&huart1, &wifi_byte, 1U);
  }
  else if (huart->Instance == USART3)
  {
    if (rs_head < (RX_CAP - 1U)) { rs_buf[rs_head] = rs_byte; rs_head++; }
    (void)HAL_UART_Receive_IT(&huart3, &rs_byte, 1U);
  }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1) { (void)HAL_UART_Receive_IT(&huart1, &wifi_byte, 1U); }
  else if (huart->Instance == USART3) { (void)HAL_UART_Receive_IT(&huart3, &rs_byte, 1U); }
}

/* ==================== 探头供电 ==================== */
static void SensorPower(int on)
{
  GPIO_PinState lv = on ? GPIO_PIN_RESET : GPIO_PIN_SET;
  HAL_GPIO_WritePin(S1_PORT, S1_PIN, lv);
  HAL_GPIO_WritePin(S2_PORT, S2_PIN, lv);
}

/* ====================== Modbus ====================== */
static uint16_t Crc16(const uint8_t *d, uint16_t len)
{
  uint16_t crc = 0xFFFFU;
  uint16_t i; uint8_t b;
  for (i = 0U; i < len; ++i)
  {
    crc ^= d[i];
    for (b = 0U; b < 8U; ++b)
    { crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0xA001U) : (uint16_t)(crc >> 1U); }
  }
  return crc;
}
static uint16_t Be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]); }

/* 读取结果里带失败原因，便于在服务器上定位 */
typedef enum
{
  RS_OK = 0,
  RS_NO_BYTES,       /* 一个字节都没收到 -> 物理层 */
  RS_BAD_CRC,        /* 收到但 CRC 不对 */
  RS_BAD_ADDR,       /* 回应的地址不对 -> 地址冲突 */
  RS_EXCEPTION,      /* 从机异常帧 0x83 */
  RS_BAD_LEN,        /* 长度/功能码/字节数不对 */
  RS_SEND_FAIL
} RsResult;

static RsResult RsReadRegs(uint8_t addr, uint16_t reg, uint16_t cnt, uint16_t *out)
{
  uint8_t req[8], resp[32];
  uint16_t crc, got = 0U;
  uint32_t t0, tlast;
  uint16_t i;
  HAL_StatusTypeDef tx;

  req[0] = addr; req[1] = 0x03U;
  req[2] = (uint8_t)(reg >> 8U); req[3] = (uint8_t)reg;
  req[4] = (uint8_t)(cnt >> 8U); req[5] = (uint8_t)cnt;
  crc = Crc16(req, 6U);
  req[6] = (uint8_t)crc; req[7] = (uint8_t)(crc >> 8U);

  RsRxClear();
  tx = HAL_UART_Transmit(&huart3, req, 8U, 500U);
  if (tx != HAL_OK) { return RS_SEND_FAIL; }

  t0 = HAL_GetTick(); tlast = t0;
  while ((HAL_GetTick() - t0) < 600U)
  {
    uint16_t len = RsRxLen();
    if (len > got) { got = len; tlast = HAL_GetTick(); }
    else if (got > 0U && (HAL_GetTick() - tlast) >= 5U) { break; }
    HAL_Delay(1U);
  }
  if (got == 0U) { return RS_NO_BYTES; }
  if (got > sizeof(resp)) { got = sizeof(resp); }
  memcpy(resp, rs_buf, got);

  if (got < 5U) { return RS_BAD_LEN; }
  if (resp[0] != addr) { return RS_BAD_ADDR; }
  if (resp[1] == 0x83U) { return RS_EXCEPTION; }
  if (resp[1] != 0x03U) { return RS_BAD_LEN; }
  if ((uint16_t)(resp[2] + 5U) != got) { return RS_BAD_LEN; }
  if (resp[2] != (uint8_t)(cnt * 2U)) { return RS_BAD_LEN; }
  crc = Crc16(resp, (uint16_t)(got - 2U));
  if (resp[got - 2U] != (uint8_t)crc || resp[got - 1U] != (uint8_t)(crc >> 8U))
  { return RS_BAD_CRC; }

  for (i = 0U; i < cnt; ++i) { out[i] = Be16(&resp[3U + (i * 2U)]); }
  return RS_OK;
}

static float DecodeDecimal(uint16_t raw, uint16_t digits)
{
  static const float f[5] = {1.0F, 10.0F, 100.0F, 1000.0F, 10000.0F};
  if (digits > 4U) { digits = 0U; }
  return (float)raw / f[digits];
}

/* ---- 地址探测：在候选地址上试着读，看哪个地址真的有探头 ---- */
typedef struct
{
  uint8_t addr;
  uint8_t found;      /* 1 = 这个地址有探头应答且 CRC 正确 */
  uint8_t func;       /* 应答的功能码：0x03 = 正常 */
  uint16_t r0;        /* 第一个寄存器的原始值，便于判断是哪颗探头 */
  RsResult err;
} AddrProbe;

/* 探测候选地址：返回探到的个数，结果填进 out[]（out_len 个） */
static uint8_t ProbeAddresses(const uint8_t *cand, uint8_t cand_len, AddrProbe *out)
{
  uint8_t i, found = 0U;
  for (i = 0U; i < cand_len; ++i)
  {
    uint16_t v = 0U;
    RsResult r = RsReadRegs(cand[i], 0x0000U, 1U, &v);
    out[i].addr  = cand[i];
    out[i].found = (r == RS_OK) ? 1U : 0U;
    out[i].func  = (r == RS_OK) ? 0x03U : 0x00U;
    out[i].r0    = (r == RS_OK) ? v : 0U;
    out[i].err   = r;
    if (r == RS_OK) { found++; }
  }
  return found;
}

/* ==================== WiFi / MQTT ==================== */
static void EspSend(const char *cmd)
{
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)cmd, (uint16_t)strlen(cmd), 2000U);
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n", 2U, 200U);
}

static int EspWaitFor(const char *expect, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  char needle[64];
  while ((HAL_GetTick() - start) < timeout_ms)
  {
    if (WifiRxLen() > 0U)
    {
      const char *p = expect;
      while (*p != '\0')
      {
        uint16_t i = 0U;
        while (*p != '\0' && *p != '|' && i < (sizeof(needle) - 1U))
        {
          if (p[0] == '\\' && p[1] == 'r') { needle[i++] = '\r'; p += 2; }
          else if (p[0] == '\\' && p[1] == 'n') { needle[i++] = '\n'; p += 2; }
          else { needle[i++] = *p++; }
        }
        needle[i] = '\0';
        if (i > 0U && strstr(wifi_buf, needle) != NULL) { return 1; }
        if (*p == '|') { p++; }
      }
    }
    HAL_Delay(5U);
  }
  return 0;
}

static int EspCmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
  WifiRxClear();
  EspSend(cmd);
  return EspWaitFor(expect, timeout_ms);
}

/* 用 AT 发布一条 MQTT 消息，retain=1（服务器随时可查最后一条）
 *
 * ★★ 关键修复：JSON 载荷里的双引号必须转义成 \" 再放进 AT 命令！
 *    乐鑫 ESP-AT 手册：内嵌双引号要用 \22（MCU 发送时写 \"）。
 *    不转义的话 AT 解析器会在第一个内嵌引号处断开 -> ERROR -> 一个字节都发不出去。
 *    另外 ESP-AT 规定【单条 AT 命令总长 <= 256 字节】，所以本函数会检查长度并拒绝超长命令。
 */
#define AT_CMD_MAX 256U

static int EspPublish(const char *topic, const char *payload)
{
  char cmd[320];
  size_t used;
  const char *p;
  int r;

  used = (size_t)snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"", topic);
  if (used == 0U || used >= sizeof(cmd)) { return 0; }

  for (p = payload; *p != '\0'; ++p)
  {
    if (*p == '"')
    {
      if (used + 3U >= sizeof(cmd)) { return 0; }
      cmd[used++] = '\\';
      cmd[used++] = '"';
    }
    else
    {
      if (used + 2U >= sizeof(cmd)) { return 0; }
      cmd[used++] = *p;
    }
  }
  cmd[used] = '\0';
  if (used + 6U >= sizeof(cmd)) { return 0; }
  (void)snprintf(cmd + used, sizeof(cmd) - used, "\",0,1");
  used = strlen(cmd);

  /* ESP-AT 硬限制：单条命令不超过 256 字节 */
  if (used >= AT_CMD_MAX) { return 0; }

  LedRun(1);
  r = EspCmd(cmd, "OK|ERROR|FAIL", 5000U);
  LedRun(0);
  if (r && strstr(wifi_buf, "OK") != NULL) { return 1; }
  return 0;
}

/* ============== 最小系统支撑 ============== */
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
  osc.PLL.PLLM = 8U; osc.PLL.PLLN = 336U; osc.PLL.PLLP = RCC_PLLP_DIV2; osc.PLL.PLLQ = 7U;
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
  SensorPower(0);
  __disable_irq();
  for (;;)
  {
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_SET);
    HAL_Delay(100U);
  }
}

/* 失败原因转成短字符串，直接写进诊断 JSON */
static const char *RsErrName(RsResult r)
{
  switch (r)
  {
    case RS_OK:         return "ok";
    case RS_NO_BYTES:   return "no_bytes";     /* 物理层：没收到任何字节 */
    case RS_BAD_CRC:    return "bad_crc";      /* 波特率/线序/干扰 */
    case RS_BAD_ADDR:   return "bad_addr";     /* 地址冲突 */
    case RS_EXCEPTION:  return "exception";    /* 从机异常帧 */
    case RS_BAD_LEN:    return "bad_len";      /* 长度/功能码/字节数 */
    case RS_SEND_FAIL:  return "send_fail";
    default:            return "?";
  }
}

/* ==================== 主程序 ==================== */
int main(void)
{
  uint8_t i;
  char cmd[512];
  char topic_data[128];
  char topic_diag[128];
  char topic_alive[128];
  char payload[320];
  char id[40];
  int n;

  uint32_t t_next;
  uint32_t cycle = 0U;
  uint32_t pub_ok = 0U, pub_fail = 0U;

  uint16_t ph_raw = 0U;
  uint16_t do_raw[4] = {0U, 0U, 0U, 0U};
  uint16_t sat_raw[2] = {0U, 0U};
  float ph = 0.0F, do_mgl = 0.0F, wtemp = 0.0F, sat = 0.0F;
  RsResult r_ph, r_do, r_sat;
  int wifi_up = 0, mqtt_up = 0;

  /* 地址探测用的候选列表（1~6，覆盖常见设置） */
  static const uint8_t cand_addr[6] = {1U, 2U, 3U, 4U, 5U, 6U};
  AddrProbe probe[6];
  uint8_t probe_found = 0U;
  char probe_json[220];

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART1_UART_Init();   /* WiFi ESP-12F */
  MX_USART3_UART_Init();   /* RS485 探头 */
  /* USART2 不初始化：你没有 USB-TTL，本程序不用串口调试 */

  (void)HAL_UART_Receive_IT(&huart1, &wifi_byte, 1U);
  (void)HAL_UART_Receive_IT(&huart3, &rs_byte, 1U);

  /* 开机两灯同闪 3 次 */
  for (i = 0U; i < 3U; ++i)
  {
    LedRun(1); LedErr(1); HAL_Delay(150U);
    LedRun(0); LedErr(0); HAL_Delay(150U);
  }

  /* [1] 探头供电 */
  SensorPower(1);

  /* 主题：全部用 STM32 唯一 ID */
  n = snprintf(id, sizeof(id), "%08lX%08lX%08lX",
    (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
  if (n <= 0) { for (;;) { HAL_Delay(1000U); } }

  (void)snprintf(topic_data, sizeof(topic_data),  "fish/device/%s/water/latest", id);
  (void)snprintf(topic_diag, sizeof(topic_diag),  "fish/device/%s/water/diag",   id);
  (void)snprintf(topic_alive, sizeof(topic_alive),"fish/device/%s/water/alive",  id);

  /* [2] 连 WiFi + MQTT */
  HAL_Delay(1500U);
  {
    int ok = 0;
    for (i = 0U; i < 3U && ok == 0; ++i)
    { ok = EspCmd("AT", "OK", 3000U); if (ok == 0) { HAL_Delay(500U); } }
    if (ok == 0) { LedErr(1); for (;;) { HAL_Delay(1000U); } }   /* 红灯常亮 = AT 不通 */
    (void)EspCmd("ATE0", "OK", 1500U);
  }
  if (!EspCmd("AT+CWMODE=1", "OK", 3000U)) { LedErr(1); for (;;) { HAL_Delay(1000U); } }

  n = snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
  if (!EspCmd(cmd, "OK|FAIL|ERROR", 30000U) || strstr(wifi_buf, "OK") == NULL)
  { LedErr(1); for (;;) { HAL_Delay(1000U); } }                   /* 红灯常亮 = 连不上热点 */
  wifi_up = 1;

  n = snprintf(cmd, sizeof(cmd), "AT+MQTTUSERCFG=0,1,\"fish-%s-w\",\"%s\",\"%s\",0,0,\"\"",
    id, MQTT_USER, MQTT_PASSWORD);
  if (!EspCmd(cmd, "OK|ERROR", 3000U) || strstr(wifi_buf, "OK") == NULL)
  { LedErr(1); for (;;) { HAL_Delay(1000U); } }

  n = snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%u,0", MQTT_HOST, (unsigned)MQTT_PORT);
  if (!EspCmd(cmd, "OK|ERROR|FAIL", 15000U) || strstr(wifi_buf, "OK") == NULL)
  { LedErr(1); for (;;) { HAL_Delay(1000U); } }
  (void)EspCmd("AT+MQTTCONN?", "OK", 3000U);
  if (strstr(wifi_buf, ",4,") == NULL && strstr(wifi_buf, ",5,") == NULL &&
      strstr(wifi_buf, ",6,") == NULL)
  { LedErr(1); for (;;) { HAL_Delay(1000U); } }
  mqtt_up = 1;

  /* [3] 预热 */
  for (i = 0U; i < WARMUP_SECONDS; ++i) { HAL_Delay(1000U); }

  /* [3.5] 地址探测：在 1~6 号地址上各试读一次，看探头到底在哪个地址上 */
  probe_found = ProbeAddresses(cand_addr, 6U, probe);
  {
    int off = 0;
    off += snprintf(probe_json + off, sizeof(probe_json) - (size_t)off,
      "{\"stage\":\"scan\",\"n\":%u,\"a\":[", (unsigned)probe_found);
    for (i = 0U; i < 6U; ++i)
    {
      off += snprintf(probe_json + off, sizeof(probe_json) - (size_t)off,
        "%s[%u,%u,%u]", (i == 0U) ? "" : ",",
        (unsigned)probe[i].addr, (unsigned)probe[i].found, (unsigned)probe[i].r0);
      if (off < 0 || (size_t)off >= sizeof(probe_json)) { break; }
    }
    if (off > 0 && (size_t)off < (sizeof(probe_json) - 2U))
    { (void)snprintf(probe_json + off, sizeof(probe_json) - (size_t)off, "]}"); }
    if (EspPublish(topic_diag, probe_json)) { pub_ok++; } else { pub_fail++; }
  }

  /* [4] 循环 */
  t_next = HAL_GetTick();

  for (;;)
  {
    /* 心跳灯：每 5 秒闪一次；有网络失败时红灯常亮 */
    LedErr((wifi_up && mqtt_up) ? (((HAL_GetTick() / 1000U) % 5U) == 0U) : 1);

    if ((int32_t)(HAL_GetTick() - t_next) < 0) { HAL_Delay(20U); continue; }
    t_next = HAL_GetTick() + (CYCLE_SECONDS * 1000U);
    cycle++;

    /* ---- 读三个量 ---- */
    r_ph  = RsReadRegs(PH_ADDR, 0x0000U, 1U, &ph_raw);
    r_do  = RsReadRegs(DO_ADDR, 0x0000U, 4U, do_raw);
    r_sat = RsReadRegs(DO_ADDR, 0x0004U, 2U, sat_raw);

    if (r_ph  == RS_OK) { ph    = (float)ph_raw / 100.0F; }
    if (r_do  == RS_OK) { do_mgl = DecodeDecimal(do_raw[0], do_raw[1]);
                          wtemp  = DecodeDecimal(do_raw[2], do_raw[3]); }
    if (r_sat == RS_OK) { sat   = DecodeDecimal(sat_raw[0], sat_raw[1]); }

    /* ---- (1) 诊断消息：每轮都发，能看到全部状态和失败原因 ----
     * ★ 长度预算（ESP-AT 单条命令硬上限 256 字节）：
     *   主题 56 + AT+MQTTPUB 框架 22 + 载荷 + 转义引号*2 + 结尾 6
     *   本载荷经主机测试实测：正常情况 ~240 字节，最坏情况（三个都报 exception）~268 字节。
     *   所以故意用最短字段名，并且 EspPublish 里会做长度检查，超限直接拒绝（不是发出去被截断）。
     *   字段：c=轮次 up=开机秒 ep/ed/es=pH/溶氧/饱和度 的失败原因
     *         rp/rd/rt/rs=原始寄存器  ok/ng=发布成功/失败累计
     */
    n = snprintf(payload, sizeof(payload),
      "{\"c\":%lu,\"ep\":\"%s\",\"ed\":\"%s\",\"es\":\"%s\","
      "\"rp\":%u,\"rd\":%u,\"rt\":%u,\"rs\":%u,\"ok\":%lu,\"ng\":%lu}",
      (unsigned long)cycle,
      RsErrName(r_ph), RsErrName(r_do), RsErrName(r_sat),
      (unsigned)ph_raw, (unsigned)do_raw[0], (unsigned)do_raw[2], (unsigned)sat_raw[0],
      (unsigned long)pub_ok, (unsigned long)pub_fail);

    if (n > 0 && (size_t)n < sizeof(payload))
    { if (EspPublish(topic_diag, payload)) { pub_ok++; } else { pub_fail++; } }

    /* ---- (2) 水质数据：三项都成功、且量程合理才发（和正式工程一致） ---- */
    if (r_ph == RS_OK && r_do == RS_OK && r_sat == RS_OK &&
        ph >= 0.0F && ph <= 14.0F && do_mgl >= 0.0F && do_mgl <= 100.0F &&
        wtemp >= -50.0F && wtemp <= 120.0F && sat >= 0.0F && sat <= 300.0F)
    {
      n = snprintf(payload, sizeof(payload),
        "{\"ph\":%.2f,\"doMgL\":%.2f,\"waterTemp\":%.2f,\"doSaturation\":%.2f}",
        (double)ph, (double)do_mgl, (double)wtemp, (double)sat);
      if (n > 0 && (size_t)n < sizeof(payload))
      { if (EspPublish(topic_data, payload)) { pub_ok++; } else { pub_fail++; } }
    }

    /* ---- (3) 心跳：每 6 轮发一次（约 60 秒） ---- */
    if ((cycle % 6U) == 0U)
    {
      n = snprintf(payload, sizeof(payload),
        "{\"up\":%lu,\"wf\":%d,\"mq\":%d,\"ok\":%lu,\"ng\":%lu}",
        (unsigned long)(HAL_GetTick() / 1000U), wifi_up, mqtt_up,
        (unsigned long)pub_ok, (unsigned long)pub_fail);
      if (n > 0 && (size_t)n < sizeof(payload))
      { if (EspPublish(topic_alive, payload)) { pub_ok++; } else { pub_fail++; } }
    }

    /* ---- (4) 发布失败累计超过 5 次，尝试重连 MQTT 一次 ---- */
    if (pub_fail >= 5U && (pub_fail % 5U) == 0U)
    {
      if (!EspCmd("AT+MQTTCONN=0,\"" MQTT_HOST "\",1883,0", "OK|ERROR|FAIL", 15000U) ||
          strstr(wifi_buf, "OK") == NULL)
      { mqtt_up = 0; }
      else { mqtt_up = 1; }
    }
  }
}
