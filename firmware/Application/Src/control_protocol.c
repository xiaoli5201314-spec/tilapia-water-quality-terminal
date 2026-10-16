/* 文件：control_protocol.c */
/* 功能：使用官方 MIT jsmn 分词，补充平坦 JSON 语法、重复键与精确业务 schema 验证。 */
#include "control_protocol.h"
#define JSMN_STRICT
#define JSMN_STATIC
#include "jsmn.h"
#include <string.h>
#include <stddef.h>
enum {
  F_V, F_ID, F_OP, F_ISSUED, F_TTL, F_MODE, F_START, F_END, F_SAMPLE,
  F_PUBLISH, F_ACTION, F_DURATION, F_TARGET, F_TYPE, F_INTERVAL,
  F_ENABLED, F_EXTEND, F_BRUSH, F_RETRACT, F_SETTLE, F_COUNT
};
#define BIT(n) (1UL << (n))
#define COMMON (BIT(F_V)|BIT(F_ID)|BIT(F_OP)|BIT(F_ISSUED)|BIT(F_TTL))
static const char *const names[F_COUNT] = {
  "v","id","op","issued","ttl","mode","start","end","sample","publish",
  "action","duration","target","type","interval","enabled","extend","brush","retract","settle"
};

/* 字符串 token 只做长度精确比较，正文里的关键词不能成为字段。 */
static bool Equal(const char *s, const jsmntok_t *t, const char *word)
{
  return t != NULL && t->type == JSMN_STRING &&
    (size_t)(t->end-t->start)==strlen(word) && memcmp(s+t->start, word, strlen(word))==0;
}
/* JSON 空白仅接受标准四种字节。 */
static int Space(const char *s, int pos, int length)
{
  while (pos<length && (s[pos]==' ' || s[pos]=='\t' || s[pos]=='\r' || s[pos]=='\n')) { ++pos; }
  return pos;
}
/* jsmn 不检查所有逗号/冒号，按 token 边界验证唯一平坦对象的完整语法。 */
static bool FlatGrammar(const char *s, int length, const jsmntok_t *t, int count)
{
  int i, pos=Space(s,0,length);
  if (count<3 || (count%2)!=1 || t[0].type!=JSMN_OBJECT ||
      pos!=t[0].start || s[pos++]!='{') { return false; }
  for (i=1; i<count; i+=2)
  {
    int j;
    pos=Space(s,pos,length);
    if (t[i].type!=JSMN_STRING || pos!=t[i].start-1 || s[pos]!='"' ||
        t[i+1].size!=0 || (t[i+1].type!=JSMN_STRING && t[i+1].type!=JSMN_PRIMITIVE))
    { return false; }
    for (j=t[i].start; j<t[i].end; ++j)
    { if ((unsigned char)s[j]<32U || s[j]=='\\') { return false; } }
    pos=Space(s,t[i].end+1,length);
    if (pos>=length || s[pos++]!=':') { return false; }
    pos=Space(s,pos,length);
    if (pos!=t[i+1].start-(t[i+1].type==JSMN_STRING ? 1 : 0)) { return false; }
    for (j=t[i+1].start; j<t[i+1].end; ++j)
    { if ((unsigned char)s[j]<32U || s[j]=='\\') { return false; } }
    pos=Space(s,t[i+1].end+(t[i+1].type==JSMN_STRING ? 1 : 0),length);
    if (i+2<count) { if (pos>=length || s[pos++]!=',') { return false; } }
  }
  if (pos>=length || s[pos++]!='}' || pos!=t[0].end) { return false; }
  return Space(s,pos,length)==length;
}
/* 只接受非负标准十进制 JSON 整数，显式检查 uint32 溢出。 */
static bool Number(const char *s, const jsmntok_t *t, uint32_t low, uint32_t high, uint32_t *out)
{
  uint32_t n=0U;
  int i;
  if (t==NULL || t->type!=JSMN_PRIMITIVE || t->end<=t->start ||
      (s[t->start]=='0' && t->end-t->start!=1)) { return false; }
  for (i=t->start; i<t->end; ++i)
  {
    uint32_t digit=(uint32_t)(s[i]-'0');
    if (s[i]<'0' || s[i]>'9' || n>(UINT32_MAX-digit)/10U) { return false; }
    n=n*10U+digit;
  }
  if (n<low || n>high) { return false; }
  *out=n; return true;
}
/* 枚举选择要求字符串类型与完整匹配。 */
static bool Choice(const char *s, const jsmntok_t *t, const char *const *words,
                   uint32_t count, uint32_t *out)
{
  uint32_t i;
  for (i=0U; i<count; ++i) { if (Equal(s,t,words[i])) { *out=i; return true; } }
  return false;
}
/* 命令 ID 只接受十二个小写十六进制字符。 */
static bool Id(const char *s, const jsmntok_t *t, char *id)
{
  int i;
  if (t==NULL || t->type!=JSMN_STRING || t->end-t->start!=12) { return false; }
  for (i=0; i<12; ++i)
  {
    char c=s[t->start+i];
    if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) { return false; }
  }
  memcpy(id,s+t->start,12U); id[12]='\0'; return true;
}

/* 按 op 建立唯一字段集合并验证业务边界，不通过 strstr 搜索 JSON。 */
bool ControlProtocol_Parse(const uint8_t *json, uint16_t length, ControlCommand *c, char ack_id[13])
{
  jsmn_parser parser;
  jsmntok_t tokens[45];
  const jsmntok_t *f[F_COUNT]={0};
  const char *s=(const char *)json;
  static const char *const ops[]={"sampling","mode","brush","actuator","plan","cleaning_config","cleaning","stop","query"};
  static const char *const modes[]={"run","pause","schedule"};
  static const char *const control_modes[]={"auto","manual"};
  static const char *const actions[]={"stop","start","extend","retract"};
  static const char *const types[]={"off","daily","interval"};
  static const char *const targets[]={"brush","actuator"};
  uint32_t seen=0U, expected=COMMON, value;
  int count, i, k;
  if (ack_id!=NULL) { ack_id[0]='\0'; }
  if (c==NULL || json==NULL || length==0U || length>CONTROL_COMMAND_MAX) { return false; }
  memset(c,0,sizeof(*c));
  for (i=0; i<length; ++i) { if (json[i]==0U) { return false; } }
  jsmn_init(&parser);
  count=jsmn_parse(&parser,s,length,tokens,45U);
  if (count<0 || !FlatGrammar(s,length,tokens,count)) { return false; }
  /* 先提取回执 ID，即使其他 schema 字段错误也能明确拒绝。 */
  for (i=1; i<count; i+=2)
  {
    if (Equal(s,&tokens[i],"id") && ack_id!=NULL) { (void)Id(s,&tokens[i+1],ack_id); }
    for (k=0; k<F_COUNT; ++k) { if (Equal(s,&tokens[i],names[k])) { break; } }
    if (k==F_COUNT || (seen&BIT(k))!=0U) { return false; }
    seen|=BIT(k); f[k]=&tokens[i+1];
  }
  if ((seen&COMMON)!=COMMON || !Id(s,f[F_ID],c->id) ||
      !Number(s,f[F_V],1U,1U,&value) || !Number(s,f[F_TTL],30U,30U,&value) ||
      !Number(s,f[F_ISSUED],0U,UINT32_MAX,&c->issued) ||
      !Choice(s,f[F_OP],ops,9U,&value)) { return false; }
  c->op=(ControlOp)value;
  switch (c->op)
  {
    case OP_SAMPLING:
      expected|=BIT(F_MODE)|BIT(F_START)|BIT(F_END)|BIT(F_SAMPLE)|BIT(F_PUBLISH);
      if (!Choice(s,f[F_MODE],modes,3U,&c->mode) ||
          !Number(s,f[F_START],0U,1439U,&c->start) || !Number(s,f[F_END],0U,1439U,&c->end) ||
          !Number(s,f[F_SAMPLE],2U,3600U,&c->sample) ||
          !Number(s,f[F_PUBLISH],5U,86400U,&c->publish) ||
          (c->mode==COLLECTION_SCHEDULE && c->start==c->end)) { return false; }
      break;
    case OP_MODE:
      expected|=BIT(F_MODE);
      if (!Choice(s,f[F_MODE],control_modes,2U,&c->mode)) { return false; }
      break;
    case OP_BRUSH: case OP_ACTUATOR: case OP_CLEANING:
      expected|=BIT(F_ACTION);
      if (!Choice(s,f[F_ACTION],actions,4U,&c->action) ||
          (c->op==OP_ACTUATOR ? (c->action==ACTION_START) :
            (c->action==ACTION_EXTEND || c->action==ACTION_RETRACT))) { return false; }
      if (c->op!=OP_CLEANING && c->action!=ACTION_STOP)
      {
        expected|=BIT(F_DURATION);
        if (!Number(s,f[F_DURATION],1U,30U,&c->duration)) { return false; }
      }
      break;
    case OP_PLAN:
      expected|=BIT(F_TARGET)|BIT(F_TYPE)|BIT(F_START)|BIT(F_INTERVAL)|BIT(F_DURATION)|BIT(F_ACTION);
      if (!Choice(s,f[F_TARGET],targets,2U,&c->target) ||
          !Choice(s,f[F_TYPE],types,3U,&c->type) ||
          !Choice(s,f[F_ACTION],actions,4U,&c->action) ||
          (c->target==0U ? c->action!=ACTION_START :
            (c->action!=ACTION_EXTEND && c->action!=ACTION_RETRACT)) ||
          !Number(s,f[F_START],0U,1439U,&c->start) ||
          !Number(s,f[F_INTERVAL],60U,86400U,&c->interval) ||
          !Number(s,f[F_DURATION],1U,30U,&c->duration)) { return false; }
      break;
    case OP_CLEANING_CONFIG:
      expected|=BIT(F_ENABLED)|BIT(F_INTERVAL)|BIT(F_EXTEND)|BIT(F_BRUSH)|BIT(F_RETRACT)|BIT(F_SETTLE);
      if (f[F_ENABLED]==NULL || f[F_ENABLED]->type!=JSMN_PRIMITIVE) { return false; }
      if (f[F_ENABLED]->end-f[F_ENABLED]->start==4 &&
          memcmp(s+f[F_ENABLED]->start,"true",4U)==0) { c->enabled=1U; }
      else if (f[F_ENABLED]->end-f[F_ENABLED]->start==5 &&
               memcmp(s+f[F_ENABLED]->start,"false",5U)==0) { c->enabled=0U; }
      else { return false; }
      if (!Number(s,f[F_INTERVAL],60U,604800U,&c->interval) ||
          !Number(s,f[F_EXTEND],1U,30U,&c->extend) || !Number(s,f[F_BRUSH],1U,30U,&c->brush) ||
          !Number(s,f[F_RETRACT],1U,30U,&c->retract) || !Number(s,f[F_SETTLE],0U,600U,&c->settle))
      { return false; }
      break;
    default: break;
  }
  return seen==expected;
}
