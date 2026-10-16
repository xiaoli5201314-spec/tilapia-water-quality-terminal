/* 文件：network_manager.c */
/* 功能：实现 Wi-Fi 优先、可选 4G 接管、故障重试和恢复切回。 */
/* 当前 WATER_ENABLE_4G=0，仅使用 Wi-Fi；4G 流程保留供后续启用。 */
#include "network_manager.h"
#include "network_config.h"
#include "board_config.h"
#include "at_command.h"
#include "actuator.h"
#include "brush_motor.h"
#include "remote_control.h"
#include <stdio.h>
#include <string.h>

typedef enum
{
  PH_WIFI_AT, PH_WIFI_MODE, PH_WIFI_JOIN, PH_WIFI_RESET_SESSION, PH_WIFI_CONFIG,
  PH_WIFI_CONNECT, PH_WIFI_VERIFY, PH_WIFI_SUBSCRIBE, PH_WIFI_CLEANUP, PH_READY_WIFI,
  PH_CELL_AT, PH_CELL_SIM, PH_CELL_ATTACH, PH_CELL_CONFIG, PH_CELL_TCP,
  PH_CELL_CONNECT, PH_CELL_VERIFY, PH_READY_CELL
} LinkPhase;

static LinkPhase phase;
static MqttLink active_link;
#if WATER_ENABLE_4G
static uint8_t wifi_failures;
#endif
static uint32_t retry_at;
static uint32_t health_at;
static uint32_t probe_at;
static bool cell_needs_reset;
static char response[384];

/* 只接受能安全放入 AT 引号字符串的配置，拒绝换行和引号注入。 */
static bool ConfigValid(void)
{
  const char *items[] = {WATER_WIFI_SSID, WATER_WIFI_PASSWORD, WATER_MQTT_HOST,
                         WATER_MQTT_USER, WATER_MQTT_PASSWORD};
  uint8_t i;
  if (WATER_MQTT_PORT == 0U) { return false; }
  for (i = 0U; i < sizeof(items) / sizeof(items[0]); ++i)
  {
    if (items[i][0] == '\0' || strlen(items[i]) > 64U ||
        strpbrk(items[i], "\"\r\n\\") != NULL) { return false; }
  }
  return true;
}

/* 同一设备的双链路分别使用唯一客户端 ID，切换期间不会互相顶线。 */
static bool BuildClientId(char *id, uint16_t capacity, MqttLink link)
{
  int n = snprintf(id, capacity, "fish-%08lX%08lX%08lX-%c",
    (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
    (unsigned long)HAL_GetUIDw2(), link == MQTT_LINK_WIFI ? 'w' : 'c');
  return n > 0 && n < capacity;
}

static bool Wifi(const char *command, uint32_t timeout)
{
  return AtCommand_Execute(BOARD_WIFI_UART, command, response,
    sizeof(response), timeout) == HAL_OK;
}
static bool Cell(const char *command, const char *event, uint32_t timeout)
{
  return AtCommand_ExecuteUntil(BOARD_4G_UART, command, event, response,
    sizeof(response), timeout) == HAL_OK;
}

/* 冒号附近容许空格，但必须解析到手册规定的已认证状态 1。 */
static bool CellConnected(void)
{
  const char *p = strstr(response, "+MQTTSTATU");
  if (p == NULL) { return false; }
  p = strchr(p, ':');
  if (p == NULL) { return false; }
  do { ++p; } while (*p == ' ');
  return p[0] == '1' && (p[1] == '\r' || p[1] == '\n' || p[1] == '\0');
}
static bool WifiConnected(void)
{
  const char *p = strstr(response, "+MQTTCONN:");
  if (p == NULL) { return false; }
  p = strchr(p, ',');
  return p != NULL && (p[1] == '4' || p[1] == '5' || p[1] == '6') &&
         (p[2] == ',' || p[2] == '\r' || p[2] == '\n');
}

static void WifiFailure(uint32_t now)
{
  RemoteControl_SetConnected(false);
  if (active_link == MQTT_LINK_WIFI) { active_link = MQTT_LINK_NONE; }
#if WATER_ENABLE_4G
  if (active_link == MQTT_LINK_4G)
  {
    phase = PH_READY_CELL;
    probe_at = now + WATER_WIFI_PROBE_MS;
    return;
  }
  if (wifi_failures < 3U) { ++wifi_failures; }
  phase = wifi_failures >= 3U ? PH_CELL_AT : PH_WIFI_AT;
  retry_at = now + 5000U;
#else
  /* 当前阶段只使用 Wi-Fi；4G 接口保留，但不因 Wi-Fi 失败而切入。 */
  phase = PH_WIFI_AT;
  retry_at = now + 5000U;
#endif
}
static void CellFailure(uint32_t now)
{
#if WATER_ENABLE_4G
  if (phase == PH_CELL_TCP || phase == PH_CELL_CONNECT ||
      phase == PH_CELL_VERIFY || phase == PH_READY_CELL)
  { cell_needs_reset = true; }
  if (active_link == MQTT_LINK_4G) { active_link = MQTT_LINK_NONE; }
  /* 4G 一直不可用时也定期重新尝试 Wi-Fi，不能永久困在蜂窝重试中。 */
  if ((int32_t)(now - probe_at) >= 0)
  {
    phase = PH_WIFI_AT;
    wifi_failures = 0U;
    retry_at = now + 5000U;
    probe_at = now + WATER_WIFI_PROBE_MS;
  }
  else
  {
    phase = PH_CELL_AT;
    retry_at = now + 15000U;
  }
#else
  /* 4G 未启用时，即使误调用此函数也只回到 Wi-Fi 重试。 */
  phase = PH_WIFI_AT;
  active_link = MQTT_LINK_NONE;
  retry_at = now + 5000U;
#endif
}

void NetworkManager_Init(void)
{
  active_link = MQTT_LINK_NONE;
  phase = PH_WIFI_AT;
#if WATER_ENABLE_4G
  wifi_failures = 0U;
#endif
  retry_at = HAL_GetTick();
  health_at = HAL_GetTick() + WATER_LINK_HEALTH_MS;
  probe_at = HAL_GetTick() + WATER_WIFI_PROBE_MS;
  cell_needs_reset = false;
  RemoteControl_SetConnected(false);
}

/* 每轮推进一个连接步骤；同步 AT 等待期间仍维护电机超时。 */
void NetworkManager_Task(void)
{
  char command[256];
  char client_id[40];
  uint32_t now = HAL_GetTick();
  int n;
  if (!ConfigValid() || AtCommand_IsBusy() ||
      (int32_t)(now - retry_at) < 0) { return; }
  switch (phase)
  {
    case PH_WIFI_AT:
      if (Wifi("AT", 1000U)) { phase = PH_WIFI_MODE; }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_MODE:
      if (Wifi("ATE0", 1000U) && Wifi("AT+CWMODE=1", 2000U)) { phase = PH_WIFI_JOIN; }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_JOIN:
      n = snprintf(command, sizeof(command), "AT+CWJAP=\"%s\",\"%s\"",
        WATER_WIFI_SSID, WATER_WIFI_PASSWORD);
      if (n > 0 && (size_t)n < sizeof(command) && Wifi(command, 20000U))
      { phase = PH_WIFI_RESET_SESSION; }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_RESET_SESSION:
      /* STM32 复位或发布误超时后，ESP 可能仍连接；清理后再配置和订阅。 */
      /* 空会话可返回 ERROR；后续配置、连接查询和订阅仍须各自成功。 */
      (void)Wifi("AT+MQTTCLEAN=0", 10000U);
      phase=PH_WIFI_CONFIG;
      break;
    case PH_WIFI_CONFIG:
      if (!BuildClientId(client_id, sizeof(client_id), MQTT_LINK_WIFI))
      { WifiFailure(now); break; }
      n = snprintf(command, sizeof(command),
        "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"",
        client_id, WATER_MQTT_USER, WATER_MQTT_PASSWORD);
      if (n > 0 && (size_t)n < sizeof(command) && Wifi(command, 3000U))
      { phase = PH_WIFI_CONNECT; }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_CONNECT:
      n = snprintf(command, sizeof(command), "AT+MQTTCONN=0,\"%s\",%u,0",
        WATER_MQTT_HOST, (unsigned)WATER_MQTT_PORT);
      if (n > 0 && (size_t)n < sizeof(command) && Wifi(command, 15000U))
      { phase = PH_WIFI_VERIFY; }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_VERIFY:
      if (Wifi("AT+MQTTCONN?", 3000U) && WifiConnected())
      {
        bool had_cell = active_link == MQTT_LINK_4G;
        active_link = MQTT_LINK_WIFI;
#if WATER_ENABLE_4G
        wifi_failures = 0U;
#endif
        health_at = HAL_GetTick() + WATER_LINK_HEALTH_MS;
        phase = had_cell ? PH_WIFI_CLEANUP : PH_WIFI_SUBSCRIBE;
      }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_SUBSCRIBE:
      n = snprintf(command, sizeof(command), "AT+MQTTSUB=0,\"%s\",0",
        RemoteControl_CommandTopic());
      if (n > 0 && (size_t)n < sizeof(command) && Wifi(command, 2000U))
      { phase=PH_READY_WIFI; RemoteControl_SetConnected(true); }
      else { WifiFailure(HAL_GetTick()); }
      break;
    case PH_WIFI_CLEANUP:
      /* 已切换活动通道；清理旧 4G 会话，断开失败也不再允许其发布。 */
      (void)Cell("AT+MDISCONNECT", NULL, 2000U);
      (void)Cell("AT+MIPCLOSE", NULL, 2000U);
      phase = PH_WIFI_SUBSCRIBE;
      break;
    case PH_READY_WIFI:
      if ((int32_t)(now - health_at) >= 0)
      {
        health_at = now + WATER_LINK_HEALTH_MS;
        if (!Wifi("AT+MQTTCONN?", 3000U) || !WifiConnected())
        { WifiFailure(HAL_GetTick()); }
      }
      break;
    case PH_CELL_AT:
      if (cell_needs_reset)
      {
        /* 上一次 TCP 已建立但 MQTT 失败，先关闭旧连接再重试。 */
        (void)Cell("AT+MDISCONNECT", NULL, 2000U);
        (void)Cell("AT+MIPCLOSE", NULL, 2000U);
        cell_needs_reset = false;
      }
      if (Cell("AT", NULL, 1500U)) { phase = PH_CELL_SIM; }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_CELL_SIM:
      /* SIM 卡必须先准备好；单独一个状态，避免应答缓冲区被下一条命令覆盖。 */
      if (Cell("ATE0", NULL, 1500U) && Cell("AT+CPIN?", NULL, 3000U) &&
          strstr(response, "READY") != NULL) { phase = PH_CELL_ATTACH; }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_CELL_ATTACH:
      /* 合宙 Air780E 的 MQTT 指令自带 TCP 连接，不需要 SIMCom 的 AT+CIPMUX。 */
      if (Cell("AT+CGATT?", NULL, 3000U) &&
          strstr(response, "+CGATT: 1") != NULL) { phase = PH_CELL_CONFIG; }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_CELL_CONFIG:
      if (!BuildClientId(client_id, sizeof(client_id), MQTT_LINK_4G))
      { CellFailure(now); break; }
      n = snprintf(command, sizeof(command), "AT+MCONFIG=\"%s\",\"%s\",\"%s\"",
        client_id, WATER_MQTT_USER, WATER_MQTT_PASSWORD);
      if (n > 0 && (size_t)n < sizeof(command) && Cell(command, NULL, 3000U))
      { phase = PH_CELL_TCP; }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_CELL_TCP:
      n = snprintf(command, sizeof(command), "AT+MIPSTART=\"%s\",%u",
        WATER_MQTT_HOST, (unsigned)WATER_MQTT_PORT);
      if (n > 0 && (size_t)n < sizeof(command) && Cell(command, "CONNECT OK", 15000U))
      { phase = PH_CELL_CONNECT; }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_CELL_CONNECT:
      if (Cell("AT+MCONNECT=1,60", "CONNACK OK", 10000U))
      { phase = PH_CELL_VERIFY; }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_CELL_VERIFY:
      if (Cell("AT+MQTTSTATU", NULL, 3000U) && CellConnected())
      {
        active_link = MQTT_LINK_4G;
        health_at = HAL_GetTick() + WATER_LINK_HEALTH_MS;
        phase = PH_READY_CELL;
      }
      else { CellFailure(HAL_GetTick()); }
      break;
    case PH_READY_CELL:
      if ((int32_t)(now - health_at) >= 0)
      {
        health_at = now + WATER_LINK_HEALTH_MS;
        if (!Cell("AT+MQTTSTATU", NULL, 3000U) || !CellConnected())
        { CellFailure(HAL_GetTick()); break; }
      }
      if ((int32_t)(now - probe_at) >= 0)
      { probe_at = now + WATER_WIFI_PROBE_MS; phase = PH_WIFI_AT; }
      break;
    default: break;
  }
}

MqttLink NetworkManager_GetActiveLink(void) { return active_link; }

void NetworkManager_ReportPublishFailure(MqttLink link)
{
  if (link != active_link) { return; }
  if (link == MQTT_LINK_WIFI) { WifiFailure(HAL_GetTick()); }
  else if (link == MQTT_LINK_4G) { CellFailure(HAL_GetTick()); }
}
/* 断开 URC 只更新状态并取消旧队列，回调中不发送 AT。 */
void NetworkManager_NotifyDisconnected(void)
{
  /* 主动清理本就会产生断开通知，不能把它当故障重新开始连接流程。 */
  if (phase==PH_WIFI_RESET_SESSION) { return; }
  WifiFailure(HAL_GetTick());
}
