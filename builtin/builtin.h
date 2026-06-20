#ifndef HCL2_BUILTIN_H
#define HCL2_BUILTIN_H

#include <stdbool.h>
#include <stddef.h>

#include "hcl2.h"

/* shared growable string buffer used by several builtins */
struct sb {
  char *p;
  size_t len, cap;
  bool oom;
};

/* shared helpers (defined in builtin/util.c) */
bool sb_put(struct sb *s, const char *d, size_t n);
bool sb_puts(struct sb *s, const char *str);
size_t u8_clen(unsigned char c);
size_t u8_runes(const char *s);
size_t u8_byteoff(const char *s, size_t r);
hcl2_value *str1(const hcl2_value *const *a, size_t n, char *e, size_t es, bool up);
hcl2_value *minmax(const hcl2_value *const *a, size_t n, char *e, size_t es, bool mx);
hcl2_value *num1(const hcl2_value *const *a, size_t n, char *e, size_t es, double (*f)(double),
                 const char *who);
int parseint_digit(char c, int base);
hcl2_value *mkstr_n(const char *p, size_t len);
bool in_cutset(char c, const char *cut);
hcl2_value *conv1(const hcl2_value *const *a, size_t n, char *e, size_t es, hcl2_type *t,
                  const char *who);
bool json_emit(const hcl2_value *v, struct sb *s);
bool push_clone(hcl2_value *out, const hcl2_value *src);
int sort_cmp(const void *x, const void *y);
bool flatten_into(hcl2_value *out, const hcl2_value *v);
hcl2_value *boollist(const hcl2_value *const *a, size_t n, char *e, size_t es, bool all,
                     const char *who);

/* regex engine (builtin/regex_engine.c) */
typedef struct hregex hregex;
hregex *hre_compile(const char *pat, char *err, size_t errsz);
void hre_free(hregex *re);
int hre_ngroups(const hregex *re);
const char *hre_group_name(const hregex *re, int g);
int hre_match(const hregex *re, const char *s, size_t len, size_t from, int *cap);
hcl2_value *hre_submatch_value(const hregex *re, const char *s, const int *cap, char *e, size_t es);

/* crypto / encoding (builtin/crypto.c) */
void hc_md5(const unsigned char *d, size_t n, unsigned char out[16]);
void hc_sha1(const unsigned char *d, size_t n, unsigned char out[20]);
void hc_sha256(const unsigned char *d, size_t n, unsigned char out[32]);
void hc_sha512(const unsigned char *d, size_t n, unsigned char out[64]);
char *hc_base64_encode(const unsigned char *d, size_t n);
unsigned char *hc_base64_decode(const char *s, size_t *outlen);
char *hc_hex(const unsigned char *d, size_t n);

/* IPv4/IPv6 helpers for cidr* (builtin/ip.c) */
bool ip_parse_cidr(const char *s, unsigned char ip[16], int *bits, int *plen, char *err, size_t es);
char *ip_format(const unsigned char *ip, int bits);
void ip_mask(unsigned char *ip, int bits, int plen);
void ip_add(unsigned char *ip, int bits, unsigned long long v);
void ip_set_field(unsigned char *ip, int start, int count, unsigned long long val);
bool ip_has_bits_below(const unsigned char *ip, int bits, int plen);
void ip_inc_at(unsigned char *ip, int plen);

/* RFC 3339 time / Go durations / formatdate (builtin/timefmt.c) */
bool rfc3339_parse(const char *s, long long *epoch, int *off, char *err, size_t es);
char *rfc3339_format(long long epoch, int off);
bool duration_parse(const char *s, long long *secs, char *err, size_t es);
char *format_date(const char *fmt, long long epoch, int off, char *err, size_t es);

/* filesystem helpers (builtin/fsutil.c) */
char *clean_path(const char *path);
char *read_file(const char *path, size_t *len, char *err, size_t es);
bool utf8_valid(const char *s, size_t n);
bool glob_match(const char *p, const char *s);

/* builtin implementations (one per file) */
hcl2_value *bi_length(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_upper(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_lower(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_join(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_split(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_min(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_max(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_abs(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_floor(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_ceil(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_signum(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_log(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_pow(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_parseint(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_chomp(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_trim(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_trimspace(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_trimprefix(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_trimsuffix(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_startswith(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_endswith(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_indent(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_substr(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_strrev(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_title(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_replace(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_concat(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_keys(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_values(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_contains(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_lookup(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_coalesce(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_tostring(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_tonumber(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_tobool(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_jsondecode(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_jsonencode(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_element(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_slice(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_reverse(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_sum(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_range(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_sort(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_distinct(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_compact(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_flatten(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_index(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_one(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_alltrue(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_anytrue(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_coalescelist(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_merge(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_zipmap(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_chunklist(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_matchkeys(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_tolist(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_toset(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_tomap(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_setunion(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_setintersection(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_setsubtract(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_setproduct(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_transpose(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_format(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_formatlist(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_csvdecode(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_regex(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_regexall(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_base64encode(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_base64decode(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_md5(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_sha1(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_sha256(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_sha512(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_base64sha256(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_base64sha512(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_uuidv5(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_uuid(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_cidrhost(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_cidrnetmask(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_cidrsubnet(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_cidrsubnets(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_timestamp(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_plantimestamp(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_timeadd(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_timecmp(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_formatdate(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_abspath(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_dirname(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_basename(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_pathexpand(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_file(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_fileexists(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_filebase64(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_fileset(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_templatefile(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_yamldecode(const hcl2_value *const *a, size_t n, char *e, size_t es);
hcl2_value *bi_yamlencode(const hcl2_value *const *a, size_t n, char *e, size_t es);

#endif
