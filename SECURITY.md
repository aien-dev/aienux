# Security policy

## Reporting a vulnerability

Report privately. Do not open a public issue, pull request or discussion about it.

1. GitHub: open this repository's Security tab and choose "Report a vulnerability". This
   creates a private advisory that only the maintainers can see. If that option is not shown,
   private vulnerability reporting has not been enabled for this repository yet; use email.
2. Email: drake@aienos.com with the subject `[aienux] security report`.

Include the commit or release you tested, the component (path), steps to reproduce, and what an
attacker gains. If the report involves a secret, say where it was exposed and do not send the
secret itself. Encrypted mail is not required; do not post proof-of-concept payloads publicly.

## Response targets

This project has one maintainer; the targets below are best effort and stated honestly.

- Acknowledgement: within 3 working days.
- Triage and severity: within 7 days of acknowledgement.
- Fix or mitigation for confirmed issues: aim for 30 days for high and critical, 90 days
  otherwise, with a status update at least every 14 days until closed.
- Disclosure: coordinated. An advisory is published with credit (unless you ask otherwise)
  once the fix is on the default branch. Default embargo 90 days, negotiable.

## Scope

In scope:

- The C kernel in `native/kernel/` (exception levels, MMU, scheduling, IPC, user-mode isolation).
- The UEFI boot path and image slots in `native/boot/` (slot selection, update, rollback).
- Capability enforcement in `native/capability/` and `crates/aienos-capability*`.
- Cryptography and key handling in `native/crypto/`, `native/sig/`, `native/m5/` and
  `crates/aienos-crypto/`: non-constant-time comparisons, missing zeroization, plaintext released
  before tag verification, AAD that differs from the layout in `AGENTS.md`.
- Store and disk code in `native/store/`, `native/disk/` and `crates/aienos-store-tool/`: torn
  writes, conflation of structural integrity, keyed authentication and anti-rollback freshness.
- Network code in `native/net/` and containment in `native/argus/`.
- `.github/workflows/` and `tests/gates/run.sh`.

Out of scope: Linux or vendor firmware in an optional compatibility island; the vendored Omega
runtime source under `native/kernel/third_party/omega/` (report to its upstream unless the issue is
in how Aienux uses it); hardware faults on machines the repository does not claim to support.

## Supported versions

Only the default branch `main`; there is no release. Reports against older commits are welcome but
fixes land on the default branch only.

## Status

Experimental and pre-alpha; the README states nothing here is physically qualified. There is no
certification and no bug bounty. See AUDIT_SCOPE.md for what to review and how to build and test
it.
