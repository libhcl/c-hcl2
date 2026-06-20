#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* IPv4 / IPv6 address helpers for the cidr* functions. Addresses are held as
 * big-endian byte arrays (4 bytes for v4, 16 for v6); `bits` is 32 or 128. */

static int parse_groups(const char *s, unsigned *out, int max) {
  if (*s == '\0')
    return 0;
  int n = 0;
  const char *p = s;
  while (*p) {
    char *end;
    long v = strtol(p, &end, 16);
    if (end == p || v < 0 || v > 0xffff || n >= max)
      return -1;
    out[n++] = (unsigned)v;
    p = end;
    if (*p == ':')
      p++;
    else if (*p != '\0')
      return -1;
  }
  return n;
}

static bool parse_v6(const char *s, unsigned char ip[16]) {
  memset(ip, 0, 16);
  const char *dc = strstr(s, "::");
  unsigned g[8];
  if (dc) {
    char head[64], tail[64];
    size_t hl = (size_t)(dc - s);
    if (hl >= sizeof head || strlen(dc + 2) >= sizeof tail)
      return false;
    memcpy(head, s, hl);
    head[hl] = '\0';
    strcpy(tail, dc + 2);
    unsigned hg[8], tg[8];
    int hn = parse_groups(head, hg, 8);
    int tn = parse_groups(tail, tg, 8);
    if (hn < 0 || tn < 0 || hn + tn > 7)
      return false;
    for (int i = 0; i < hn; i++) {
      ip[2 * i] = (unsigned char)(hg[i] >> 8);
      ip[2 * i + 1] = (unsigned char)(hg[i] & 0xff);
    }
    for (int i = 0; i < tn; i++) {
      int pos = 8 - tn + i;
      ip[2 * pos] = (unsigned char)(tg[i] >> 8);
      ip[2 * pos + 1] = (unsigned char)(tg[i] & 0xff);
    }
    return true;
  }
  if (parse_groups(s, g, 8) != 8)
    return false;
  for (int i = 0; i < 8; i++) {
    ip[2 * i] = (unsigned char)(g[i] >> 8);
    ip[2 * i + 1] = (unsigned char)(g[i] & 0xff);
  }
  return true;
}

bool ip_parse_cidr(const char *s, unsigned char ip[16], int *bits, int *plen, char *err,
                   size_t es) {
  const char *slash = strchr(s, '/');
  if (slash == NULL) {
    everr(err, es, "cidr: expected address/prefix");
    return false;
  }
  char addr[64];
  size_t al = (size_t)(slash - s);
  if (al >= sizeof addr) {
    everr(err, es, "cidr: address too long");
    return false;
  }
  memcpy(addr, s, al);
  addr[al] = '\0';
  char *end;
  long pl = strtol(slash + 1, &end, 10);
  if (*end != '\0' || pl < 0) {
    everr(err, es, "cidr: invalid prefix length");
    return false;
  }
  memset(ip, 0, 16);
  if (strchr(addr, ':')) {
    if (!parse_v6(addr, ip) || pl > 128) {
      everr(err, es, "cidr: invalid IPv6 prefix");
      return false;
    }
    *bits = 128;
  } else {
    unsigned o[4];
    int consumed = 0;
    if (sscanf(addr, "%u.%u.%u.%u%n", &o[0], &o[1], &o[2], &o[3], &consumed) != 4 ||
        addr[consumed] != '\0' || o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255 || pl > 32) {
      everr(err, es, "cidr: invalid IPv4 prefix");
      return false;
    }
    for (int i = 0; i < 4; i++)
      ip[i] = (unsigned char)o[i];
    *bits = 32;
  }
  *plen = (int)pl;
  return true;
}

char *ip_format(const unsigned char *ip, int bits) {
  char buf[64];
  if (bits == 32) {
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return strdup(buf);
  }
  unsigned g[8];
  for (int i = 0; i < 8; i++)
    g[i] = ((unsigned)ip[2 * i] << 8) | ip[2 * i + 1];
  int best = -1, bestlen = 0, cur = -1, curlen = 0;
  for (int i = 0; i < 8; i++) {
    if (g[i] == 0) {
      if (cur < 0)
        cur = i, curlen = 1;
      else
        curlen++;
      if (curlen > bestlen)
        bestlen = curlen, best = cur;
    } else
      cur = -1, curlen = 0;
  }
  if (bestlen < 2)
    best = -1;
  char *q = buf;
  for (int i = 0; i < 8; i++) {
    if (best != -1 && i >= best && i < best + bestlen) {
      if (i == best)
        *q++ = ':';
      continue;
    }
    if (i != 0)
      *q++ = ':';
    q += sprintf(q, "%x", g[i]);
  }
  if (best != -1 && best + bestlen == 8)
    *q++ = ':';
  *q = '\0';
  return strdup(buf);
}

/* zero every bit at or beyond position `plen` (network address). */
void ip_mask(unsigned char *ip, int bits, int plen) {
  for (int b = plen; b < bits; b++)
    ip[b / 8] &= (unsigned char)~(1 << (7 - (b % 8)));
}

/* add v to the address (big-endian, at the low end). */
void ip_add(unsigned char *ip, int bits, unsigned long long v) {
  int carry = 0;
  for (int i = bits / 8 - 1; i >= 0; i--) {
    unsigned sum = ip[i] + (unsigned)(v & 0xff) + (unsigned)carry;
    ip[i] = (unsigned char)(sum & 0xff);
    carry = (int)(sum >> 8);
    v >>= 8;
    if (v == 0 && carry == 0)
      break;
  }
}

/* write the low `count` bits of val into the bit field [start, start+count). */
void ip_set_field(unsigned char *ip, int start, int count, unsigned long long val) {
  for (int i = 0; i < count; i++) {
    int bitpos = start + i;
    int bit = (int)((val >> (count - 1 - i)) & 1);
    int byte = bitpos / 8, off = 7 - (bitpos % 8);
    if (bit)
      ip[byte] |= (unsigned char)(1 << off);
    else
      ip[byte] &= (unsigned char)~(1 << off);
  }
}

bool ip_has_bits_below(const unsigned char *ip, int bits, int plen) {
  for (int b = plen; b < bits; b++)
    if (ip[b / 8] & (1 << (7 - (b % 8))))
      return true;
  return false;
}

/* add one block of size 2^(bits-plen): increment at bit (plen-1). */
void ip_inc_at(unsigned char *ip, int plen) {
  for (int i = plen - 1; i >= 0; i--) {
    int byte = i / 8, off = 7 - (i % 8);
    if (ip[byte] & (1 << off))
      ip[byte] &= (unsigned char)~(1 << off); /* carry */
    else {
      ip[byte] |= (unsigned char)(1 << off);
      return;
    }
  }
}
