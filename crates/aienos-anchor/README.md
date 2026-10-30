# aienos-anchor

Aien's anchor ([aienix#5](https://github.com/aien-dev/aienix/issues/5)): one small, trusted place that holds checked facts. When Aien is unsure, or its own memory disagrees with something, it asks the anchor, and a signed answer wins.

- **Append-only chain.** Each entry names the digest of the one before it. Removing, reordering or editing any entry breaks the chain, and a broken log is refused whole.
- **Only trusted keys write.** Every entry is Ed25519-signed (`verify_strict`, the vendored `ed25519-dalek` 2.2.0 already reviewed in `docs/ED25519_DEPENDENCY_REVIEW.md`) over a domain-prefixed body. The model holds no admission key, so it cannot add or change facts.
- **Three answers:** `Known` (value, source, entry number, signer), `Retracted`, or `Unknown`. No guessing.
- **Retraction is an entry,** never a deletion; you can only retract a fact that is currently known.
- **Bounded, byte-by-byte decoding:** key up to 256 bytes, value up to 4096, source up to 256. `no_std` + `alloc`.

Status: A1 core (library + tests). Not yet running on the Pi: storage on the device and the query path for Aien are next (A1b, A2).

Tests: `cargo test -p aienos-anchor` (11 tests: admission, unknown, newest-wins, retraction rules, model key refused, tampered value, out-of-order, broken chain, bounds, export/replay, tampered/truncated/shortened log, foreign signature domain). Mutants checked: removing the signature check fails 2 tests; removing the chain check fails 1.
