# Native SHA256 reuse

Exact upstream `seed/native/sha256.om` from omega-osc-manual-migration commit
8f8be9a676b94197e5cb363d2dcbca113e208f09, last file commit at read-only checkout
9b072eb492e106d23f7eed984c5645251601adee. Its core comes from native Omega TRN1
12251e3403d42931ada27dc1c54b3b610954be5d. `sha256.om` preserves that whole file.
`sha256-core.om` is its byte-exact prefix before the handwritten standalone stream
interface comment (no algorithm changes). Build-time concatenation is not a new
language import/compiler feature. No primitive was reimplemented. Qualification
here includes independently constructed SHA-sealed record bytes and hash mutants.
The upstream AGPL licence is preserved as OMEGA-LICENSE. Imported sources retain
SPDX identifiers. No keyed authentication/freshness claim follows from these tests.

Compiler is immutable omega-bridge-native-development commit
0fc7d8662cb799e7c206b17c18ea4c3ec2b8fea9, omc SHA256
191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9.
Fresh source-only bootstrap from git archive at that pin passed, stage1==stage2,
and its omc hash matched the brief's compiler. No C parent or compiler edits.

# Certificate verification reuse

All three files below are public-input verification code. They contain no signing or private-key code.

- `cert-p256.om`: the verification subset of `om/p256/p256.om` from aien-dev/spark-rsi-mv, local
  checkout commit bba3877510daeb8a0e070a781a689bcf1952b6f6, source SHA256
  92da68007af091ccfbe01aa845be779502130e02bf57e9550332d669905db958. The names were prefixed
  (`cp2`/`C2`), the caller now supplies the digest, and signing, HMAC, RFC 6979 and public-key
  derivation were removed.
- `../tls-p384.om`: the same Montgomery and complete-addition code, adapted to 12 limbs with the
  SEC 2 v2 section 2.5.1 P-384 constants.
- `cert-sha384.om`: `om/sha512/sha512.om` from aien-dev/omnet
  a14ff7efc94ea56530a61e0d91675dc970e7d806, source SHA256
  069a54b75484221bc7dbacfbea744ca9002c295f4f34f2c3905b0776450e3ce2, with the FIPS 180-4
  section 5.3.4 SHA-384 IV. Only the first 48 output bytes are used.

These sources are Apache-2.0. Their licence text is `CERT-APACHE-LICENSE`.

`cert-root.om` holds the public trust anchor read from the local OS store on 2026-10-25:
`/etc/ssl/certs/Sectigo_Public_Server_Authentication_Root_E46.pem`, DER SHA256
c90f26f0fb1b4018b22227519b5ca2b53e2ca5b3be5cf18efe1bef47380c5383. This is a root anchor, not a
peer, leaf or SPKI pin.
