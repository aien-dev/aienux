# Pipe primitive vectors (aienos-mv#428)

Machine-checkable cases for the native pipe primitive (`native/kernel/core/pipe.c`), one file per case, read by
`native/kernel/tests/test_pipe.c`. Contract: OSH_PLATFORM_ABI v1 DRAFT (aien-protocols-mv `specs/osh-platform/`,
read at `fb5fca98e667`), sections 4.4, 8.1, 8.5, 8.6 and 9. Authored from that text and this kernel's choices; the
choices the draft leaves open are marked UNVERIFIED in `core/pipe.h`. Plain text so the corpus can later move to
`specs/osh-platform/vectors/` next to the shell-core vectors (same quoted-byte escapes as their `expected.txt`).

## File `pNNN_<name>.vec`

- `case <name>` and `kind positive|negative` are required.
- `#` starts a comment. Tokens are separated by spaces.
- `<q>` is a double-quoted byte string: `\xHH` any byte, `\"` and `\\` themselves. NUL and bytes >= 0x80 are allowed.
- Two capability tables exist, `A` and `B` (two principals), both empty at the start; the pool has 4 pipes of 65536 bytes.
- Every step line is `<command> => <ERROR> [n=<N>] [data=<q>]`. `<ERROR>` is a section 8.6 name (`OK`, `DENIED`,
  `REVOKED`, `STALE`, `INVALID_ARG`, `LIMIT`, `INTERRUPTED`, `BROKEN_PIPE`, `NOT_SUPPORTED`, `WOULD_BLOCK`, ...).
  `n=` is the byte count moved (or, for `release`, the capabilities closed); `data=` the exact bytes read.

| command | meaning |
|---|---|
| `create <R> <W> <T>` | PIPE_CREATE in table T; names the read and write end |
| `write <H> <q>` | non-blocking STREAM_WRITE |
| `fill <H> <count> <hex byte>` | non-blocking STREAM_WRITE of `count` copies of one byte |
| `read <H> <max>` | non-blocking STREAM_READ |
| `bread <H> <max> <action>` / `bwrite <H> <q> <action>` | blocking read / write; the first time it parks, the peer runs `<action>` (`none`, or one `write`, `read`, `close`, `revoke`, `cancel` command, which must return OK); a second park gives up INTERRUPTED (a stalled peer) |
| `close <H>` | STREAM_CLOSE |
| `dup <H> <NEW> <T> <rights>` | STREAM_DUP into table T; rights letters `r` READ, `w` WRITE, `m` MAP, `g` GRANT, `d` DERIVE, `v` REVOKE |
| `revoke <H>` / `cancel <H>` | revoke the pipe / interrupt that end |
| `release <T>` | close every pipe capability in table T (section 12 cleanup) |
| `frame <T> <slice> <cell>...` | raw platform call through the versioned op table; `<slice>` is `-` or `<q>`; cells are hex, or `$<H>` for a handle in register form; the peer table is the other one |
| `check <item>...` | `empty` (both tables and the pool hold nothing), or `<counter>=<N>` over the pool totals: `bytes_written bytes_read writes reads max_resident would_block waits wakeups cancellations revocations broken_pipe eof refused`, and `pipes` (live pipes) |

A handle name keeps its value after `close`, so a later use of it tests a stale handle. Each case must end with every
capability released: the runner fails a case whose tables or pool are not empty at the end of the file.
