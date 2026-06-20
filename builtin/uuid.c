#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* uuid(): a random (version 4) UUID. Entropy from /dev/urandom, falling back
 * to the C PRNG. Non-deterministic by design (like Terraform's uuid). */
hcl2_value *bi_uuid(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  (void)a;
  if (n != 0) {
    everr(e, es, "uuid() takes no arguments");
    return NULL;
  }
  unsigned char b[16];
  FILE *f = fopen("/dev/urandom", "rb");
  bool got = false;
  if (f != NULL) {
    got = fread(b, 1, 16, f) == 16;
    fclose(f);
  }
  if (!got) {
    static bool seeded = false;
    if (!seeded) {
      srand((unsigned)time(NULL));
      seeded = true;
    }
    for (int i = 0; i < 16; i++)
      b[i] = (unsigned char)(rand() & 0xff);
  }
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40); /* version 4 */
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); /* RFC 4122 variant */

  char out[37];
  snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
           b[14], b[15]);
  return hcl2_string(out);
}
