# iMac DMAR firmware inventory

This implements the first discovery step of
[aienux #411](https://github.com/aien-dev/aienux/issues/411). It does not
implement or qualify device DMA confinement. Existing device startup continues
unchanged after the inventory, including when the table is absent or refused.
The image must not be treated as safe for sensitive memory or unrestricted
remote operations on the strength of this inventory.

## Implementation

`native/kernel/core/dmar.c` reads one explicitly bounded byte buffer using
the format in the [Intel VT-d Architecture Specification, revision 5.0,
sections 8.1 through 8.4](https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf).
It checks the signature, declared size, checksum, revision, reserved fields,
entry ordering, nested lengths, device/function bounds, remapping-unit
alignment and reserved-region range/alignment. It retains DRHD units, their
device scopes, and RMRR reserved ranges with their scopes. Other structure
types are counted and skipped by declared length, as the specification requires.
Skipping them is not a declaration that their requirements are supported.

Storage is fixed: at most 64 KiB of table bytes, eight units, sixteen regions,
64 scopes across both owner types and eight device/function pairs per path.
Oversized inventories return a capacity error. Unknown scope types or table
revisions return unsupported. Every failure clears the output, including
failures after valid entries. There is no allocation, register access or
device configuration in the parser.

`native/kernel/x86/dmar_x86.c` locates the table with the existing checked ACPI
lookup, inventories it before PCI drivers start, and prints `dmar:` diagnostics.
It retains segment numbers and complete paths. A one-hop path has a direct
source ID. A longer path remains unresolved until each bridge's secondary bus
has been read; the last path element alone is insufficient. A subhierarchy
also needs downstream enumeration. Reserved regions are reported as firmware
requirements, never treated as granted windows.

The report ends with `inventory-only register_access=none; DMA confinement
NOT QUALIFIED`. Absence, invalid data and refusal also explicitly state that
confinement is not qualified. The optional lab diagnostic report allowlists
these lines. Its existing unauthenticated status remains unchanged. Default
images still contain no network report.

## Verification

The host test `native/kernel/tests/test_dmar.c` covers a multi-segment fixture,
multi-hop device paths, an IOAPIC, a reserved region, an unknown structure,
every truncated prefix, corrupt fields, capacity exhaustion and a 64-bit
address boundary. The same test runs with address and undefined-behavior
sanitizers through the normal kernel host targets.

`scripts/qemu_ck_imac_kernel_test.sh` checks that the default simulated machine
reports the absent table without claiming protection. Setting
`AIENOS_IMAC_DMAR=1` adds QEMU's Intel IOMMU device and requires a parsed unit,
IOAPIC scope and inventory-only marker under both existing CPU models. These
are firmware discovery tests. They neither enable remapping nor test blocked
DMA. The existing read-only disk checks and deliberate W^X failures remain.

`scripts/ck_imac_report_test.sh` checks that inventory diagnostics survive the
allowlist while chat, keys and tokens remain excluded. Its lab build uses the
existing explicit test-key option solely for this hosted test. No hardware
staging image is produced by that script.

## Remaining work

Read and preserve the actual iMac's map on an explicitly authorized, attended
boot. Resolve all relevant device paths against its PCI topology and account
for reserved memory and any additional DMAR structures. Then implement and
test domain construction, approved windows, invalidation, fault handling and
fail-closed driver lifecycles. Physical out-of-window DMA and device recovery
tests are still required by #411. No isolation claim follows from a successful
inventory or an emulator boot.
