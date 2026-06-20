#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_cidrhost(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_NUMBER) {
    everr(e, es, "cidrhost() needs (prefix, hostnum)");
    return NULL;
  }
  if (a[1]->num < 0) {
    everr(e, es, "cidrhost() host number cannot be negative");
    return NULL;
  }
  unsigned char ip[16];
  int bits, plen;
  if (!ip_parse_cidr(a[0]->str, ip, &bits, &plen, e, es))
    return NULL;
  ip_mask(ip, bits, plen);
  ip_add(ip, bits, (unsigned long long)a[1]->num);
  char *s = ip_format(ip, bits);
  if (s == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(s);
  free(s);
  return r;
}
