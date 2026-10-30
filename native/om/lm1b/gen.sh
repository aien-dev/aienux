#!/bin/sh
# gen.sh: expand lm1b.in.om into lm1b.om (POSIX sh + awk, existing-tool class).
# Generated pieces: the 49 model pools and the code that names each pool (globals cannot be
# passed to functions in Omega), the pinned manifest text, the tensor slot table and the digest.
# Inputs: lm1b.in.om, sha256.om (vendored), manifest.txt (from oracle/manifest-dump).
set -eu
cd "$(dirname "$0")"
FSIZE=807694368          # model bytes (pinned)
POOL=16777216            # bytes per pool (Omega global limit: 16,777,216 elements)
SHA=3f5a22426976ab26cfe84dba63c1d08391717abb1af893e10f1b2968d862dcc1
awk -v fsize="$FSIZE" -v pool="$POOL" -v sha="$SHA" '
function npools() { return int((fsize + pool - 1) / pool) }
function plen(k) { return (k < npools() - 1) ? pool : fsize - (npools() - 1) * pool }
function slot(name,   a, n, kinds, i) {
  if (name == "token_embd.weight") return 144
  if (name == "output_norm.weight") return 145
  if (name == "rope_freqs.weight") return 146
  n = split(name, a, ".")
  if (n != 4 || a[1] != "blk" || a[4] != "weight") { print "gen.sh: unknown tensor " name > "/dev/stderr"; exit 2 }
  split("attn_q attn_k attn_v attn_output attn_norm ffn_norm ffn_gate ffn_up ffn_down", kinds, " ")
  for (i = 1; i <= 9; i++) if (kinds[i] == a[3]) return a[2] * 9 + i - 1
  print "gen.sh: unknown kind " name > "/dev/stderr"; exit 2
}
FILENAME == "manifest.txt" {
  man = man $0 "\\n"
  if ($1 == "t") { s = slot($2); if (s in seen) { print "gen.sh: slot twice " s > "/dev/stderr"; exit 2 }; seen[s] = 1; tsl = tsl sprintf("%03d", s) }
  next
}
FILENAME == "sha256.om" { shasrc = shasrc $0 "\n"; next }
/^@@GEN_GLOBALS@@$/ { for (k = 0; k < npools(); k++) printf "global m%d uchar %d\n", k, plen(k); next }
/^@@GEN_MANIFEST@@$/ {
  printf "text man \"%s\"\n", man
  printf "text tslot \"%s\"\n", tsl
  printf "text sha \"%s\"\n", sha
  next
}
/^@@SHA256@@$/ { printf "%s", shasrc; next }
/^@@GEN_CPSEG@@$/ {
  print "fn cpseg p ulong o ulong d ulong k ulong to void"
  for (k = 0; k < npools(); k++) {
    printf "if eq p %d\nfor i ulong k\nput rb add d i get m%d add o i\nend\nret\nend\n", k, k
  }
  print "trap 10"; print "end"; next
}
/^@@GEN_FB@@$/ {
  print "fn fb a ulong to uchar"; print "p = shr a 24"; print "o = and a 16777215"
  for (k = 0; k < npools(); k++) printf "if eq p %d\nret get m%d o\nend\n", k, k
  print "trap 10"; print "end"; next
}
/^@@GEN_LOAD@@$/ {
  for (k = 0; k < npools(); k++) {
    printf "lo = ulong 0\nwhile lt lo %d bound 999\n", plen(k)
    printf "r = pread int 3 m%d lo sub %d lo long add lo %d\n", k, plen(k), k * pool
    printf "if le r 0\nrefuse WREAD\nend\nlo = add lo ulong r\nend\n"
  }
  next
}
/^@@GEN_HASH@@$/ {
  for (k = 0; k < npools(); k++) {
    nb = int(plen(k) / 64); tl = plen(k) - nb * 64
    printf "for b ulong %d\nho = mul b 64\nfor i ulong 64\nput hb i get m%d add ho i\nend\nhblock\nend\n", nb, k
    printf "put hmeta 1 add get hmeta 1 %d\n", nb * 64
    if (tl > 0) printf "for i ulong %d\nhok = hpush get m%d add %d i\nif not hok\nrefuse WSHA\nend\nend\n", tl, k, nb * 64
  }
  next
}
/@@/ { print "gen.sh: unexpanded marker " $0 > "/dev/stderr"; exit 2 }
{ print }
' manifest.txt sha256.om lm1b.in.om > lm1b.om.tmp
mv lm1b.om.tmp lm1b.om
echo "gen.sh: wrote lm1b.om ($(wc -l < lm1b.om) lines)"
