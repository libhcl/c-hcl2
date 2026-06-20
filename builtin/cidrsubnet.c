#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_cidrsubnet(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_NUMBER ||
      a[2]->kind != HCL2_NUMBER) {
    everr(e, es, "cidrsubnet() needs (prefix, newbits, netnum)");
    return NULL;
  }
  if (a[1]->num < 0 || a[2]->num < 0) {
    everr(e, es, "cidrsubnet() newbits and netnum cannot be negative");
    return NULL;
  }
  unsigned char ip[16];
  int bits, plen;
  if (!ip_parse_cidr(a[0]->str, ip, &bits, &plen, e, es))
    return NULL;
  int newbits = (int)a[1]->num;
  if (plen + newbits > bits) {
    everr(e, es, "cidrsubnet() not enough bits to extend prefix");
    return NULL;
  }
  unsigned long long netnum = (unsigned long long)a[2]->num;
  if (newbits < 63 && netnum >= (1ULL << newbits)) {
    everr(e, es, "cidrsubnet() netnum too large for newbits");
    return NULL;
  }
  ip_mask(ip, bits, plen);
  ip_set_field(ip, plen, newbits, netnum);
  char *addr = ip_format(ip, bits);
  if (addr == NULL)
    return NULL;
  char buf[80];
  snprintf(buf, sizeof buf, "%s/%d", addr, plen + newbits);
  free(addr);
  return hcl2_string(buf);
}
