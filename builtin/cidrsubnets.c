#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_cidrsubnets(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n < 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "cidrsubnets() needs (prefix, newbits...)");
    return NULL;
  }
  unsigned char cur[16];
  int bits, plen;
  if (!ip_parse_cidr(a[0]->str, cur, &bits, &plen, e, es))
    return NULL;
  ip_mask(cur, bits, plen);
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 1; i < n; i++) {
    if (a[i]->kind != HCL2_NUMBER || a[i]->num < 0) {
      everr(e, es, "cidrsubnets() newbits must be non-negative numbers");
      hcl2_value_free(out);
      return NULL;
    }
    int nb = (int)a[i]->num;
    int newprefix = plen + nb;
    if (newprefix > bits) {
      everr(e, es, "cidrsubnets() not enough bits to extend prefix");
      hcl2_value_free(out);
      return NULL;
    }
    /* align the cursor up to the boundary of this subnet's size */
    if (ip_has_bits_below(cur, bits, newprefix)) {
      ip_mask(cur, bits, newprefix);
      ip_inc_at(cur, newprefix);
    }
    char *addr = ip_format(cur, bits);
    if (addr == NULL) {
      hcl2_value_free(out);
      return NULL;
    }
    char buf[80];
    snprintf(buf, sizeof buf, "%s/%d", addr, newprefix);
    free(addr);
    hcl2_value *v = hcl2_string(buf);
    if (v == NULL || !hcl2_tuple_push(out, v)) {
      hcl2_value_free(v);
      hcl2_value_free(out);
      return NULL;
    }
    /* advance past this subnet */
    ip_inc_at(cur, newprefix);
  }
  return out;
}
