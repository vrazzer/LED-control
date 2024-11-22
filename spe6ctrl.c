/* spe6ctrl.c
 * Copyright 2024 Vraz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* notes:
   compile via: gcc -g -O0 spe6ctrl.c -o spe6ctrl -lbluetooth
   only tested on one spe630e
   why does x2902 query gives two different responses?
   investigate 0x01: returns differing single by based on parm
   investivate 0x61: what do var34/var35 control?
   investivate 0x62: must be more than mode change
   how to translate video rgb to led rgb?

   general use: len=controllers/meter, speed=5
   good remote mode+effect (modified by speed+len+dir):
     3:1 (rainbow wave) 3:4 (rainbox chase) 3:112 (rainbow stack) 3:146 (rainbow fade)
     3:119 (rbw snake) 3:121 (rgw snake) 
     7:1 (static), 7:2 (chase fwd), 7:4 (chase out), 7:17 (fade thru)
   custom palettes for holidays (work in progress):
     4:193:49:28 4:235:186:56 4:162:107:53 4:98:48:4 4:189:184:80 (fall/thanksgiving for 20 chip/m)
     7:0xff:0xd0:0x00 6:0xff:0x68:0x16 7:0x04:0xb1:0x00 (thanksgiving w/green for 20 chip/m)
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

// gatt bluetooth types (no public header-- search "att-types.h")
#define GATT_ERR_RSP          0x01
#define GATT_MTU_REQ          0x02
#define GATT_MTU_RSP          0x03
#define GATT_FIND_INFO_REQ    0x04
#define GATT_FIND_INFO_RSP    0x05
#define GATT_FIND_BY_TYPE_VAL_REQ 0x06
#define GATT_FIND_BY_TYPE_VAL_RSP 0x07
#define GATT_READ_BY_TYPE_REQ 0x08
#define GATT_READ_BY_TYPE_RSP 0x09
#define GATT_READ_REQ         0x0a
#define GATT_READ_RSP         0x0b
#define GATT_READ_BLOB_REQ    0x0c
#define GATT_READ_BLOB_RSP    0x0d
#define GATT_READ_MULT_REQ    0x0e
#define GATT_READ_MULT_RSP    0x0f
#define GATT_READ_BY_GRP_TYPE_REQ  0x10
#define GATT_READ_BY_GRP_TYPE_RSP  0x11
#define GATT_WRITE_REQ        0x12
#define GATT_WRITE_RSP        0x13
#define GATT_WRITE_CMD        0x52
#define GATT_SIGNED_WRITE_CMD  0xD2
#define GATT_PREP_WRITE_REQ   0x16
#define GATT_PREP_WRITE_RSP   0x17
#define GATT_EXEC_WRITE_REQ   0x18
#define GATT_EXEC_WRITE_RSP   0x19
#define GATT_HAND_VAL_NOTIFY  0x1B
#define GATT_HAND_VAL_IND     0x1D // IND??
#define GATT_HAND_VAL_CONF    0x1E // CONF (CONFIG?)

#define CONN_TIMEOUT 10

// table of device fingerprints from 2902 query
const static struct {
  const char *kind;
  uint8_t resp[16];
} _ident[] = {
    "SP630E-0", { 10, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00 },
    "SP630E-1", { 10, 0x00, 0x00, 0x0f, 0x00, 0x01, 0x00, 0x15, 0x00, 0x00, 0x00 },
};

// query returns 77+rcnt*2 bytes (97 on factory fresh unit)
static struct sp630e {
  uint8_t u0[1];     // 0x00: convenience uint8 pointer
  uint8_t u1[4];     // 0x01: 0x01
  uint8_t fw[8];     // 0x05: firmware version (V3.0.08 on my unit)
  uint8_t u13[1];    // 0x0d: 0x80=factory 0x86=tester
  uint8_t u14[1];    // 0x0e: 0x01=factory 0x03=tester
  uint8_t u15[1];    // 0x0f: 0x03
  uint8_t u16[1];    // 0x10: 0x00
  uint8_t u17[1];    // 0x11: 0x3c=factory 0x14=tester
  uint8_t coexist;   // 0x12: allow rgb+white (why?)
  uint8_t reboot;    // 0x13: power state on reboot (0=off, 1=on, 2=resume)

  uint8_t u20[1];    // 0x14: 0x02
  uint8_t u21[1];    // 0x15: 0x4b=factory 0x3b=tester
  uint8_t u22[1];    // 0x16: 0x00
  uint8_t power;     // 0x17: current power (0=off, 1=on)
  uint8_t loop;      // 0x18: loop through effects (0=off, 1=on)
  uint8_t u25[1];    // 0x19: 0x00=factory 0x04=tester
  uint8_t mode;      // 0x1a: display mode (1/2=static, 3/4=dynamic, 5/6=sound, 7=custom)
  uint8_t effect;    // 0x1b: effect within mode (over 130 effects for mode 3)
  uint8_t u28[1];    // 0x1c:
  uint8_t level;     // 0x1d: rgb color level
  uint8_t white;     // 0x1e: white-led intensity
  uint8_t rgb[3];    // 0x1f: static rgb color
  uint8_t var34;     // 0x22: changed by 0x61/0x5e for unknown purpose
  uint8_t var35;     // 0x23: changed by 0x61/0x5e for unknown purpose
  uint8_t speed;     // 0x24: effect speed (1..10)
  uint8_t len;       // 0x25: effect length (1..150)
  uint8_t dir;       // 0x26: effect direction (0/1)
  uint8_t gain;      // 0x27: microphone gain (0=disable, 1..255=gain)

  uint8_t mic;       // 0x28: sound trigger (0=internal microphone, 1=pulse request)
  uint8_t rgb2[3];   // 0x29: rgb changed by 0x57 for unknown purpose
  uint8_t var44;     // 0x2c: set by 0x5e
  uint8_t var45;     // 0x2d: set by 0x5e
  uint8_t u46[2];    // 0x2e:
  struct {           // custom (mode 7) length+rgb data
    uint8_t len;     // pixel length (1..?)
    uint8_t rgb[3];  // color rgb
  } cust[7];         // 0x30: list of seven custom colors
  uint8_t rcnt;      // 0x4c: number of remote control mode+effects (factory=10)
  uint8_t rme[20];   // 0x4d: remote-control mode+effect pairs (10 pairs max)
} _sp = { 0 }, _pr = { 0 };

static struct {
  int off;           // current offset into parm query (0=not in progress)
  int cnt;           // count of queries (0=no query yet)
} _qs = { 0 };

// simple command table
struct command {
  const char *cmd;  // command name
  const char *adj;  // adjustments made during request-send (a=add prefix byte, p=pad request)
  uint8_t min;      // minimum user parms
  uint8_t max;      // maximum user parms
  uint8_t code;     // request code
  const char *parm; // parm list
  const char *help; // help text
};

static const struct command cmdlist[] = {
  "query",   "",   1, 1, 0x02, "<0=short|1=long>", "parameter query",
  "onoff", "a1",   4, 4, 0x08, "<1..4=effect> <1..3=speed> <pixels-high> <pixels-low>", "effect used during power-on/off",
  "coexist", "",   1, 1, 0x0a, "<0=separate|1=combined>", "some kind of color+white blending?",
  "reboot",  "",   1, 1, 0x0b, "<0=off|1=on|2=resume>", "action when power applied (seems unworking)",
  "power",   "",   1, 1, 0x50, "<0=off|1=on>", "set power on/off (and trigger on-off effect)",
  "level",   "",   2, 2, 0x51, "<0=color|1=white> <0..255>", "change the color/white level (all modes)",
  "rgb",     "",   4, 4, 0x52, "<0..255=r> <0..255=g> <0..255=b> <0..255=level>", "set static mode color/intensity",
  "mode",    "",   1, 2, 0x53, "<0=pause|1..7=mode> [<1..x=effect>]", "set mode and optionally effect (effect count varies by mode)",
  "speed",   "",   1, 1, 0x54, "<1..10=speed>", "set effect speed (all modes)",
  "len",     "",   1, 1, 0x55, "<1..150=len>", "set dynamic-mode effect length",
  "dir",     "",   1, 1, 0x56, "<0=left|1=right>", "change dynamic-mode effect direction",
  "rgb2",    "",   3, 3, 0x57, "<0..255=r> <0..255=g> <0..255=b>", "change rgb value 0x29 for unknown purpose",
  "loop",    "",   1, 1, 0x58, "<0=off|1=on>", "loop through mode effects",
  "mic",     "",   1, 1, 0x59, "<0=internal|1=pulse-command>", "internal microphone or sequence of pulse commands",
  "gain",    "",   1, 1, 0x5a, "<0=disable|1..?=mic-gain>", "set microphone gain or enable/disable pulse",
  "pulse",   "",   0, 0, 0x5b, "", "send sound pulse for music effects (parameters seem ignored)",
  "remote",  "",   2,20, 0x5c, "<1..7=mode> <1..x=effect> ...", "set up to 10 remote-control mode+effect pairs",
  "play",    "",   1, 1, 0x5d, "<0=pause|1=play>", "pause/play effects for dynamic/sound mode",
  "bulk",    "",   1,12, 0x5e, "<1..7=mode> <1..4=effect> <1..255=level> <1..10=speed> <1..99=length> <0=left|1=right> <0..255=var44> <0.255=var45> <0..255=r> <0..255=g> <0..255=b> <0..255=var34> <0..255=var35>", "bulk set parms",
  "static", "a1a1a255a1a1a0a0a0", 3, 3, 0x5e, "<0..255=r> <0..255=g> <0..255=b>", "atomic change to static",
  "dynamic","a3", 5, 5, 0x5e, "<1..130=effect> <1..255=level> <1..10=speed> <1..99=length> <0=left|1=right>", "atomic change to dynamic",
  "music", "a5",  5, 5, 0x5e, "<1..130=effect> <1..255=level> <1..10=speed> <1..99=length> <0=left|1=right>", "atomic change to dynamic",
  "var34",   "",   1, 2, 0x61, "<0..255=var34> <0..255=var35>", "changes var34/var35 but actual purpose unknown",
  "mode2",   "",   1, 1, 0x62, "<0=pause|1..7=mode>", "change mode without changing effect (same as single-parm mode command?)",
  "custom","a1p0", 4,28, 0x63, "<1..20=len:r:g:b> [up to 6 additional]", "set custom pattern (mode 7 to animate)",
  "type",  "a1",   1, 1, 0x6a, "<1=pwm-mono|2=spi-mono|3=pwm-cct|4=spi-cct|5=pwm-rgb|6=spi-rgb|7=pwm-rgbw|8=spi-rgbw|9=pwm1+spi>", "set led string type",
  "order",   "",   1, 1, 0x6b, "<0=brg|1=bgr|2=rbg|3=gbr|4=rgb|5=grb>", "set led order (works during animation)",
  "ref",     "",   3, 3, 0x6c, "<0..255=led1> <0..255=led2> <0..255=led3>", "show static color without order correction (likely for setup)",
  "set",   "a0",   1, 1, 0x51, "<0..255=level>", "set the level (all modes)",
  "inc",  "a0m",   1, 1, 0x51, "<0..255=level>", "increase level unless already higher (must query first)",
  "dec",  "a0m",   1, 1, 0x51, "<0..255=level>", "decrease level unless already lower (must query first)",
  "", "", 0, 0, 0
};

// configuration display format
const struct {
  void *pr, *sp; int len; char *key, *fmt;
} format[] = {
  &_pr.fw, &_sp.fw, 8, "fw", "%.8s",
  NULL, NULL, 0, NULL, "\n",
  &_pr.power, &_sp.power, 1, "power", "%d",
  &_pr.reboot, &_sp.reboot, 1, "reboot", "%d",
  &_pr.mode, &_sp.mode, 1, "mode", "%d",
  &_pr.effect, &_sp.effect, 1, "effect", "%d",
  &_pr.speed, &_sp.speed, 1, "speed", "%d",
  &_pr.len, &_sp.len, 1, "len", "%d",
  &_pr.dir, &_sp.dir, 1, "dir", "%d",
  &_pr.loop, &_sp.loop, 1, "loop", "%d",
  &_pr.rgb, &_sp.rgb, 3, "rgb", "%02x:%02x:%02x",
  &_pr.var34, &_sp.var34, 1, "var34", "%d",
  &_pr.var35, &_sp.var35, 1, "var35", "%d",
  &_pr.level, &_sp.level, 1, "level", "%d",
  &_pr.white, &_sp.white, 1, "white", "%d",
  &_pr.gain, &_sp.gain, 1, "gain", "%d",
  &_pr.mic, &_sp.mic, 1, "mic", "%d",
  &_pr.rgb2, &_sp.rgb2, 3, "rgb2", "%02x:%02x:%02x",
  &_pr.var44, &_sp.var44, 1, "var44", "%d",
  &_pr.var45, &_sp.var45, 1, "var45", "%d",
  NULL, NULL, 0, NULL, "\n",
  &_pr.cust[0], &_sp.cust[0], 4, "cust0", "%d/%02x:%02x:%02x",
  &_pr.cust[1], &_sp.cust[1], 4, "cust1", "%d/%02x:%02x:%02x",
  &_pr.cust[2], &_sp.cust[2], 4, "cust2", "%d/%02x:%02x:%02x",
  &_pr.cust[3], &_sp.cust[3], 4, "cust3", "%d/%02x:%02x:%02x",
  &_pr.cust[4], &_sp.cust[4], 4, "cust4", "%d/%02x:%02x:%02x",
  &_pr.cust[5], &_sp.cust[5], 4, "cust5", "%d/%02x:%02x:%02x",
  &_pr.cust[6], &_sp.cust[6], 4, "cust6", "%d/%02x:%02x:%02x",
  NULL, NULL, 0, NULL, "\n",
  &_pr.rcnt, &_sp.rcnt, 1, "rcnt", "%d",
  &_pr.rme[0],  &_sp.rme[0],  2, "rme0", "%d:%d",
  &_pr.rme[2],  &_sp.rme[2],  2, "rme1", "%d:%d",
  &_pr.rme[4],  &_sp.rme[4],  2, "rme2", "%d:%d",
  &_pr.rme[6],  &_sp.rme[6],  2, "rme3", "%d:%d",
  &_pr.rme[8],  &_sp.rme[8],  2, "rme4", "%d:%d",
  &_pr.rme[10], &_sp.rme[10], 2, "rme5", "%d:%d",
  &_pr.rme[12], &_sp.rme[12], 2, "rme6", "%d:%d",
  &_pr.rme[14], &_sp.rme[14], 2, "rme7", "%d:%d",
  &_pr.rme[16], &_sp.rme[16], 2, "rme8", "%d:%d",
  &_pr.rme[18], &_sp.rme[18], 2, "rme9", "%d:%d",
  NULL, NULL, 0, NULL, NULL
};

// send commands to controller (cmdline format: cmd <arg1> <arg2> ... <argn>)
void cmdline(char *line, int sock)
{
  // shortcut
  if (line[0] == '?')
    strcpy(line, "query 29");

  // parse input into whitespace separated values
  int ac = 0;
  char *av[32] = { NULL }, *tok = NULL;
  for (av[ac++] = strtok_r(line, "\n\"\'= ", &tok); av[ac] = strtok_r(NULL, "\n\"\': ", &tok); ++ac)
    if (ac+1 >= sizeof(av)/sizeof(av[0])) break;
  av[ac] = NULL;

  // handle help special
  if (strcmp(av[0], "help") == 0) {
    printf("parameters can be decimal or 0x-prefixed hexidecimal\n");
    printf("#<request> <parm1> [<parm2> ...] (send a raw request)\n");
    for (int i = 0; cmdlist[i].cmd[0] != 0; ++i)
      printf("%s %s (%s)\n", cmdlist[i].cmd, cmdlist[i].parm, cmdlist[i].help);
    return;
  }

  // find matching request (name + number-of-args)
  const struct command arb = { "", "", 1, 1, strtol(line+1, NULL, 0) };
  const struct command *cmd = (line[0] == '#') ? &arb : NULL;
  for (int i = 0; cmdlist[i].cmd[0] != 0; ++i) {
    if ((strcmp(av[0], cmdlist[i].cmd) == 0) && (ac >= cmdlist[i].min+1) && (ac <= cmdlist[i].max+1)) {
      cmd = cmdlist+i;
      break;
    }
  }
  if (cmd == NULL)
    return;

  // enable notify prior to first query
  if ((strcmp(av[0], "query") == 0) && (_sp.fw[0] == 0)) {
    uint8_t req[] = { GATT_WRITE_REQ, 0x0f, 0x00, 0x01, 0x00 };
    int rc = send(sock, req, sizeof(req), 0);
    usleep(100*1000);
  }

  uint8_t req[48] = { GATT_WRITE_REQ, 0x0e, 0x00, 0x53, cmd->code, 0x00, 0x01, 0x00, 0 };
  uint8_t *add = req+8;
  char *adj = (char *)cmd->adj;
  // allow add-in byte(s)
  while (*adj == 'a')
    add[++(*add)] = strtol(++adj, &adj, 0);
  for (int i = 1; i < ac; ++i)
    add[++(*add)] = strtol(av[i], NULL, 0);
  // allow padding
  while ((*adj == 'p') && (*add < cmd->max))
    add[++(*add)] = strtol(adj+1, NULL, 0);

  // check existing level prior to inc/dec
  if ((strcmp(av[0], "inc") == 0) && (_sp.fw[0] != 0) && (_sp.level > add[2]))
    return;
  if ((strcmp(av[0], "dec") == 0) && (_sp.fw[0] != 0) && (_sp.level < add[2]))
    return;

  int len = (add-req)+1+add[0];
  int rc = send(sock, req, len, 0);
  fprintf(stderr, "send(%d): ", len);
  for (int i = 0; i < len; ++i)
    fprintf(stderr, "%02x ", req[i]);
  fprintf(stderr, "(rc=%d)\n", rc);
  if ((rc > 0) && (req[4] == 0x02)) {
    _qs.off = -1;
  }
}

// process incoming packet
void receive(const uint8_t *rcvbuf, int rcvlen)
{
  // parse device config (first segment determines width)
  if ((rcvlen > 8) && (rcvbuf[0] == GATT_HAND_VAL_NOTIFY) && (rcvbuf[3] == 0x53) && (rcvbuf[4] == 0x02)) {
    static int wid = 0;
    int seg = rcvbuf[7];
    int len = rcvbuf[8];
    if (seg == 0)
      _qs.off = 0;
    if (_qs.off+len <= sizeof(_sp))
      memcpy(&_sp.u0+_qs.off, rcvbuf+9, len);
    // must receive at least 77 bytes (rcnt)
    if ((_qs.off += len) < 77)
      return;
    // use rcnt to calc actual length
    if (_qs.off < 77+_sp.rcnt*2)
      return;
    _qs.off = 0;

    // show parms/changes after final query segment
    char out[4096];
    if (!_qs.cnt++)
      memcpy(&_pr, &_sp, sizeof(_pr));

    const char *key[256] = { NULL } ;
    for (int i = 0; format[i].fmt != NULL; ++i) {
      uint8_t *pr8 = format[i].pr;
      uint8_t *sp8 = format[i].sp;
      if ((pr8 == NULL) || (sp8 == NULL))
        continue;
      for (int j = 0; j < format[i].len; ++j) {
        key[sp8-_sp.u0+j] = format[i].key;
      }
    }

    len = 0;
    for (int i = 0; i < sizeof(_sp); ++i)
      if (_pr.u0[i] != _sp.u0[i]) {
        if (key[i] != NULL)
          len += snprintf(out+len, sizeof(out)-len, "(%s)", key[i]);
        len += snprintf(out+len, sizeof(out)-len, "0x%02x:%02x->%02x ", i, _pr.u0[i], _sp.u0[i]);
      }
    if (len > 0)
      fprintf(stderr, "diff: %s\n", out);

    len = 0;
    char pr[16], sp[16];
    for (int i = 0; format[i].fmt != NULL; ++i) {
      uint8_t *pr8 = format[i].pr;
      uint8_t *sp8 = format[i].sp;
      if ((pr8 == NULL) || (sp8 == NULL)) {
        len += snprintf(out+len, sizeof(out)-len, "%s", format[i].fmt);
        continue;
      } else if (strchr(format[i].fmt, 's') != NULL) {
        snprintf(pr, sizeof(pr), format[i].fmt, format[i].pr);
        snprintf(sp, sizeof(sp), format[i].fmt, format[i].sp);
      } else {
        snprintf(pr, sizeof(pr), format[i].fmt, pr8[0], pr8[1], pr8[2], pr8[3]);
        snprintf(sp, sizeof(sp), format[i].fmt, sp8[0], sp8[1], sp8[2], sp8[3]);
      }
      if (strcmp(pr, sp) != 0)
        len += snprintf(out+len, sizeof(out)-len, "%s=%s->%s ", format[i].key, pr, sp);
      else
        len += snprintf(out+len, sizeof(out)-len, "%s=%s ", format[i].key, sp);
    }
    if (len > 0)
      printf("%s\n", out);
    memcpy(&_pr, &_sp, sizeof(_pr));
  }
}

int main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s bt-addr [timeout] [--cmd=\"parm(s)\"] [--cmd=\"parm(s)\"] ... [-I(nteractive)]\n", argv[0]);
    for (int i = 0; cmdlist[i].cmd[0] != 0; ++i) {
      char cmd[256];
      snprintf(cmd, sizeof(cmd), "  --%s=\"%s\"", cmdlist[i].cmd, cmdlist[i].parm);
      fprintf(stderr, "%-40s  %s\n", cmd, cmdlist[i].help);
      //fprintf(stderr, "  --%s=\"%s\" (%s)\n", cmdlist[i].cmd, cmdlist[i].parm, cmdlist[i].help);
    }
    exit(0);
  }

  int argi = 2;
  time_t lasttime = (argc >= 3) && (argv[2][0] >= '1') && (argv[2][0] <= '9') ? time(NULL)+atoi(argv[argi++]) : time(NULL)+60;
  const char *kind = NULL;
  for (int sock = -1, tty = 0; time(NULL) <= lasttime;) {
    // handle socket init/connect
    if (sock < 0) {
      kind = NULL;
      memset(&_sp, 0, sizeof(_sp));
      memset(&_qs, 0, sizeof(_qs));
      sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
      if (sock < 0) {
        fprintf(stderr, "socket error: %s (%d)\n", strerror(errno), errno);
        usleep(1000*1000);
        continue;
      }

      // bluetooth requires binding prior to connect
      struct sockaddr_l2 addr;
      memset(&addr, 0, sizeof(addr));
      addr.l2_family = AF_BLUETOOTH;
      addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
      addr.l2_cid = htobs(4);
      if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        fprintf(stderr, "bind error: %s (%d)\n", strerror(errno), errno);

      // connect at low security
      struct bt_security sec;
      memset(&sec, 0, sizeof(sec));
      sec.level = BT_SECURITY_LOW;
      if (setsockopt(sock, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0)
        fprintf(stderr, "setsockopt error: %s (%d)\n", strerror(errno), errno);

      // setup destination bluetooth address
      memset(&addr, 0, sizeof(addr));
      addr.l2_family = AF_BLUETOOTH;
      addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
      addr.l2_cid = htobs(4);
      str2ba(argv[1], &addr.l2_bdaddr);

      // non-blocking connect+poll to control timeout
      int block = fcntl(sock, F_GETFL, 0);
      fcntl(sock, F_SETFL, block|O_NONBLOCK);
      connect(sock, (struct sockaddr *)&addr, sizeof(addr));
      struct pollfd pfd = { .fd = sock, .events = POLLOUT, .revents = 0 };
      time_t conntime = lasttime-time(NULL);
      if (conntime > CONN_TIMEOUT)
        conntime = CONN_TIMEOUT;
      fprintf(stderr, "connect %s (timeout=%d)\n", argv[1], conntime);
      poll(&pfd, 1, conntime*1000);
      if (pfd.revents != POLLOUT)
        fprintf(stderr, "connect error: revents=%02x\n", pfd.revents);
      fcntl(sock, F_SETFL, block);
    }

    // wait for packet (or console input during interactive mode)
    struct pollfd pfd[2] = { {.fd=sock, .events=POLLIN, .revents=0 }, {.fd=0, .events=POLLIN, .revents=0} };
    if (poll(pfd, 1+tty, 250) < 0) {
       fprintf(stderr, "poll error: %s (%d)\n", strerror(errno), errno);
       continue;
    }

    // if poll timed out and unidentified device, query it
    // (can happen multiple times as sp630e goes unresponsive periodically)
    if (!pfd[0].revents && !tty && (kind == NULL)) {
      fprintf(stderr, "identify 0x2902\n");
      usleep(100*1000);
      uint8_t req[] = { GATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0xff, 0xff, 0x02, 0x29 };
      if (send(sock, req, sizeof(req), 0) < 0) {
        fprintf(stderr, "identify error: %s (%d)\n", strerror(errno), errno);
        close(sock);
        sock = -1;
        continue;
      }
    }

    // process any incoming packet
    if (pfd[0].revents) {
      uint8_t rcvbuf[1024];
      int rcvlen = read(sock, rcvbuf, sizeof(rcvbuf));
      if (rcvlen < 0) {
        fprintf(stderr, "read error: %s (%d)\n", strerror(errno), errno);
        close(sock);
        sock = -1;
        continue;
      }

      // show meaningful packets (skip single byte response acknowledgements)
      if ((rcvlen > 1) || (rcvbuf[0] != GATT_WRITE_RSP)) {
        fprintf(stderr, "recv(%d):", rcvlen);
        for (int i = 0; i < rcvlen; ++i)
          fprintf(stderr, " %02x", rcvbuf[i]);
        if ((rcvbuf[rcvlen-1] > ' ') && (rcvbuf[rcvlen-1] < 127))
          fprintf(stderr, " (%c)", rcvbuf[rcvlen-1]);
        fprintf(stderr, "\n");
      }

      // handle identify response
      if ((rcvlen > 4) && (rcvbuf[0] == GATT_READ_BY_TYPE_RSP)) {
        for (int i = 0; i < sizeof(_ident)/sizeof(_ident[0]); ++i) {
          const uint8_t *resp = _ident[i].resp;
          if ((rcvlen == resp[0]+4) && (memcmp(rcvbuf+4, resp+1, resp[0]) == 0)) {
            kind = _ident[i].kind;
            fprintf(stderr, "found %s\n", kind);
            break;
          }
        }
      }

      // handle other receive
      receive(rcvbuf, rcvlen);
    }

    // process interactive console
    if (pfd[1].revents) {
      char line[256];
      fgets(line, sizeof(line), stdin);
      if (line[0] >= ' ')
        cmdline(line, sock);
    }

    // process command-line parms
    if ((kind != NULL) && (_qs.off == 0) && (argi < argc)) {
      char line[4096];
      snprintf(line, sizeof(line), "%s", argv[argi++]);
      if ((line[0] == '-') && (line[1] == '-')) {
        cmdline(line+2, sock);
        // next cycle waits for the response
      }
    }

    // if done with commands, exit or enable tty control
    if (!tty & (argi == argc)) {
      if ((argc > 2) && (strcmp(argv[argi-1], "-I") != 0))
        break;
      fprintf(stderr, "interactive mode (timeout disabled)\n");
      lasttime += 86400;
      tty = 1;
    }
  }
  exit(argi < argc ? EXIT_FAILURE : EXIT_SUCCESS);
}

