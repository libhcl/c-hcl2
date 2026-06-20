#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_cidrnetmask(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "cidrnetmask() needs (prefix)");
    return NULL;
  }
  unsigned char ip[16];
  int bits, plen;
  if (!ip_parse_cidr(a[0]->str, ip, &bits, &plen, e, es))
    return NULL;
  if (bits != 32) {
    everr(e, es, "cidrnetmask() requires an IPv4 prefix");
    return NULL;
  }
  unsigned char m[4] = {0};
  for (int b = 0; b < plen; b++)
    m[b / 8] |= (unsigned char)(1 << (7 - (b % 8)));
  char buf[16];
  snprintf(buf, sizeof buf, "%u.%u.%u.%u", m[0], m[1], m[2], m[3]);
  return hcl2_string(buf);
}
