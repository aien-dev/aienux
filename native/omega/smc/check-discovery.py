#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Development-only checker. No runtime driver, firmware access or port I/O.

Builds synthetic ACPI bytes as an independent test oracle, compiles the actual
Omega parser and protocol, and runs ARM/Linux + QEMU/x86 Linux. Refuses a stale
or unpinned compiler. All generated files/receipts go to a fresh external path.
"""
import argparse
import hashlib
import json
import pathlib
import platform
import shutil
import struct
import subprocess
import sys

PIN = "7d5b824f0c854063b851b7f1002ea9cb735c9f9e3ce96e3393ca6f4a46f5bfb9"
ROOT = pathlib.Path(__file__).resolve().parent
OK, ABSENT, MALFORMED, UNSUPPORTED, AMBIGUOUS, DISABLED, LIMIT = range(7)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pkg(body):
    for width in range(1, 5):
        size = len(body) + width
        if width == 1 and size < 64:
            return bytes([size]) + body
        if width > 1 and size < (1 << (4 + 8 * (width - 1))):
            return bytes([((width - 1) << 6) | (size & 15)]) + bytes(
                (size >> (4 + 8 * i)) & 255 for i in range(width - 1)
            ) + body
    raise ValueError("oversized fixture package")


def integer(n):
    if n in (0, 1):
        return bytes([n])
    if n < 256:
        return b"\x0a" + bytes([n])
    return b"\x0c" + struct.pack("<I", n)


def buffer(data, declared=None):
    return b"\x11" + pkg(integer(len(data) if declared is None else declared) + data)


def name(key, value):
    return b"\x08" + key + value


def method(key, body, flags=0):
    return b"\x14" + pkg(key + bytes([flags]) + body)


def device(body, path=b"SMC0"):
    return b"\x5b\x82" + pkg(path + body)


def scope(body, path=b"\\_SB_"):
    return b"\x10" + pkg(path + body)


def io(base=0x300, length=32, decode=1):
    return b"\x47" + bytes([decode]) + struct.pack("<HHBB", base, base, 1, length)


def template(items=None, checksum=False):
    raw = (io() if items is None else items) + b"\x79\0"
    if checksum:
        raw = raw[:-1] + bytes([(-sum(raw)) & 255])
    return raw


def fields(resources=None, sta=15, methods=False, hid=b"APP0001"):
    result = name(b"_HID", b"\x0d" + hid + b"\0")
    if sta is not None:
        result += (method(b"_STA", b"\xa4" + integer(sta)) if methods
                   else name(b"_STA", integer(sta)))
    raw = template() if resources is None else resources
    result += (method(b"_CRS", b"\xa4" + buffer(raw)) if methods
               else name(b"_CRS", buffer(raw)))
    return result


def table(body, signature=b"DSDT"):
    header = bytearray(36)
    header[:4] = signature
    struct.pack_into("<I", header, 4, 36 + len(body))
    header[8] = 2
    result = header + body
    result[9] = (-sum(result)) & 255
    return bytes(result)


def frame(tables):
    return struct.pack("<I", len(tables)) + b"".join(
        struct.pack("<I", len(t)) + t for t in tables
    )


def scenarios():
    good = table(scope(device(fields())))
    fake = device(fields())
    anc = io() + b"\x22\x40\0" + b"\x86\x09\0\x01" + struct.pack("<II", 0x100000, 65536)
    cases = [
        ("literal-scope", [good], OK),
        ("constant-return-methods", [table(device(fields(methods=True)))], OK),
        ("ancillary-irq-memory", [table(device(fields(template(anc))))], OK),
        ("nonzero-resource-checksum", [table(device(fields(template(checksum=True))))], OK),
        ("functioning-without-ui", [table(device(fields(sta=11)))], OK),
        ("siblings-preserve-path", [table(scope(device(fields(), b"SMC0") + device(fields(hid=b"OTHER00"), b"OTHR")))], OK),
        ("absolute-scope-restores-parent", [table(scope(scope(name(b"X___", integer(1)), b"\\ABCD") + device(fields())))], OK),
        ("dual-name-scope", [table(scope(device(fields()), b"\\\x2e_SB_PCI0"))], OK),
        ("multi-name-scope", [table(scope(device(fields()), b"\\\x2f\x02_SB_PCI0"))], OK),
        ("parent-path-scope", [table(scope(scope(device(fields()), b"^PCI0")))], OK),
        ("buffer-payload-not-code", [table(name(b"DATA", buffer(fake)))], ABSENT),
        ("method-payload-not-code", [table(method(b"NOOP", fake))], ABSENT),
        ("string-payload-not-code", [table(name(b"TEXT", b"\x0dAPP0001_HID_CRS\0"))], ABSENT),
        ("empty-static-table", [table(b"")], ABSENT),
        ("non-smc-device", [table(device(fields(hid=b"OTHER00")))], ABSENT),
        ("duplicate-same-table", [table(device(fields()) + device(fields(), b"SMC1"))], AMBIGUOUS),
        ("duplicate-other-ssdt", [good, table(device(fields(), b"SMC1"), b"SSDT")], AMBIGUOUS),
        ("disabled-duplicate", [good, table(device(fields(sta=0), b"SMC1"), b"SSDT")], AMBIGUOUS),
        ("duplicate-declaration", [table(device(fields() + name(b"_STA", integer(15))))], AMBIGUOUS),
        ("namespace-alias-duplicate", [table(scope(device(fields())) + scope(device(fields()), b"\\_SB_"))], AMBIGUOUS),
        ("disabled", [table(device(fields(sta=0)))], DISABLED),
        ("not-functioning", [table(device(fields(sta=3)))], DISABLED),
        ("missing-status-is-unsupported", [table(device(fields(sta=None)))], UNSUPPORTED),
        ("unknown-status-bits", [table(device(fields(sta=47)))], UNSUPPORTED),
        ("dynamic-status", [table(device(name(b"_HID", b"\x0dAPP0001\0") + method(b"_STA", b"\xa4DYN0")))], UNSUPPORTED),
        ("dynamic-crs-not-first-buffer", [table(device(name(b"_HID", b"\x0dAPP0001\0") + name(b"_STA", integer(15)) + method(b"_CRS", buffer(template()) + b"\xa4\x00")))], UNSUPPORTED),
        ("return-buffer-plus-code", [table(device(name(b"_HID", b"\x0dAPP0001\0") + name(b"_STA", integer(15)) + method(b"_CRS", b"\xa4" + buffer(template()) + b"\x00")))], UNSUPPORTED),
        ("serialized-resource-method", [table(device(name(b"_HID", b"\x0dAPP0001\0") + name(b"_STA", integer(11)) + name(b"BUF0", buffer(template())) + method(b"_CRS", b"\xa4BUF0", flags=8)))], UNSUPPORTED),
        ("buffer-size-mismatch", [table(device(name(b"_HID", b"\x0dAPP0001\0") + name(b"_STA", integer(15)) + name(b"_CRS", buffer(template(), declared=1))))], UNSUPPORTED),
        ("dynamic-hid", [table(device(method(b"_HID", b"\xa4\x0dAPP0001\0")))], UNSUPPORTED),
        ("compressed-hid-unsupported", [table(device(name(b"_HID", integer(1234))))], UNSUPPORTED),
        ("fixedio-decode-not-admitted", [table(device(fields(template(b"\x4b\0\x03\x20"))))], UNSUPPORTED),
        ("ten-bit-alias-not-admitted", [table(device(fields(template(io(decode=0)))))], UNSUPPORTED),
        ("reserved-io-bits", [table(device(fields(template(io(decode=2)))))], MALFORMED),
        ("unsupported-range", [table(device(fields(template(io(base=0x301)))))], UNSUPPORTED),
        ("zero-range", [table(device(fields(template(io(length=0)))))], MALFORMED),
        ("wrapping-range", [table(device(fields(template(io(base=65535, length=32)))))], MALFORMED),
        ("second-io-range", [table(device(fields(template(io() + io()))))], AMBIGUOUS),
        ("bad-irq-flags", [table(device(fields(template(io() + b"\x23\x40\0\x80"))))], MALFORMED),
        ("empty-irq", [table(device(fields(template(io() + b"\x22\0\0"))))], MALFORMED),
        ("bad-memory-info", [table(device(fields(template(io() + b"\x86\x09\0\x02" + struct.pack("<II", 0, 1)))))], MALFORMED),
        ("truncated-large-descriptor", [table(device(fields(io() + b"\x86\x09")))], MALFORMED),
        ("bad-resource-checksum", [table(device(fields(template()[:-1] + b"\x01")))], MALFORMED),
        ("trailing-template-bytes", [table(device(fields(template() + b"\0")))], MALFORMED),
        ("missing-endtag", [table(device(fields(io())))], MALFORMED),
        ("bad-endtag-length", [table(device(fields(io() + b"\x78")))], UNSUPPORTED),
        ("dependent-alternatives", [table(device(fields(template(io() + b"\x30"))))], UNSUPPORTED),
        ("unknown-after-valid-table", [good, table(b"\xff", b"SSDT")], UNSUPPORTED),
        ("malformed-after-valid-table", [good, b"x"], MALFORMED),
        ("truncated-package", [table(b"\x5b\x82\x40")], MALFORMED),
        ("package-crosses-parent", [table(b"\x5b\x82\x3fSMC0")], MALFORMED),
        ("unsupported-module-code", [table(b"\xa0\x02\x00")], UNSUPPORTED),
        ("declaration-capacity", [table(b"".join(name(f"A{i:03d}".encode(), integer(1)) for i in range(129)))], LIMIT),
        ("table-capacity", [table(b"", b"SSDT")] * 17, LIMIT),
    ]
    bad = bytearray(good)
    bad[9] ^= 1
    cases.append(("table-checksum", [bytes(bad)], MALFORMED))
    bad = bytearray(good)
    bad[:4] = b"XXXX"
    bad[9] = 0
    bad[9] = (-sum(bad)) & 255
    cases.append(("table-signature", [bytes(bad)], MALFORMED))
    bad = bytearray(good)
    struct.pack_into("<I", bad, 4, len(good) + 1)
    cases.append(("declared-table-truncation", [bytes(bad)], MALFORMED))
    return cases


def checked_run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, timeout=60, **kwargs)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--omc", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--captured-acpi", type=pathlib.Path)
    args = parser.parse_args()
    compiler = args.omc.resolve()
    if digest(compiler.read_bytes()) != PIN:
        parser.error("compiler digest differs from independent candidate pin")
    if args.out.exists():
        parser.error("output path must be new; historical receipts are never overwritten")
    args.out.mkdir(parents=True)
    qemu = shutil.which("qemu-x86_64")
    if not qemu:
        parser.error("qemu-x86_64 missing (NOT_RUN)")
    units = {p: (ROOT / p).read_text() for p in
             ("protocol.om", "resource.om", "discovery.om", "discovery-fixture.om", "diagnostic.om")}
    base = "\n".join(units[p] for p in ("protocol.om", "resource.om", "discovery.om", "discovery-fixture.om"))
    mutants = {
        "table-checksum": ("if ne sum 0\nret dsfail DSMAL", "if false\nret dsfail DSMAL"),
        "candidate-duplicates": ("if ne get Dsc 2 0\nret DSAMBIG", "if false\nret DSAMBIG"),
        "template-terminal": ("if ne sub limit pos 2\nret DSMAL", "if false\nret DSMAL"),
        "opaque-method": ("put Dsv 2 last\nret DSOK", "put Dsv 2 add p 1\nret DSOK"),
        "decode-width": ("if ne get Dsres 2 16\nret DSUNSUP", "if false\nret DSUNSUP"),
    }
    cases = scenarios()
    records = []
    builds = []
    failed = []
    for target, runner in (("arm", []), ("x86", [qemu])):
        prefix = "target x86linux\n" if target == "x86" else ""
        for variant, body in [("original", base)] + [
            (key, base.replace(old, new)) for key, (old, new) in mutants.items()
        ]:
            if variant != "original" and base.count(mutants[variant][0]) != 1:
                raise RuntimeError(f"non-unique mutant {variant}")
            source = args.out / f"{target}-{variant}.om"
            binary = args.out / f"{target}-{variant}"
            source.write_text(prefix + body)
            result = checked_run([str(compiler)], input=source.read_bytes())
            (args.out / f"{target}-{variant}.err").write_bytes(result.stderr)
            build = {"target": target, "variant": variant, "compile_status": result.returncode,
                     "source_sha256": digest(source.read_bytes()), "stderr": result.stderr.decode(errors="replace")}
            builds.append(build)
            if result.returncode:
                failed.append(f"compile:{target}:{variant}")
                continue
            binary.write_bytes(result.stdout)
            binary.chmod(0o700)
            build["binary_sha256"] = digest(result.stdout)
            observed_failures = []
            controls = []
            for label, tables, expected in cases:
                result = checked_run(runner + [str(binary)], input=frame(tables))
                payload = result.stdout
                outputs = struct.unpack("<7Q", payload[:56]) if len(payload) == 57 else None
                matches = (result.returncode == expected and outputs is not None
                           and payload[56] == 4 and not result.stderr)
                if matches and expected == OK:
                    matches = outputs[:3] == (0x300, 32, 16)
                    if matches:
                        data = tables[outputs[3] - 1]
                        matches = data[outputs[4]:outputs[4] + 2] == b"\x5b\x82" and data[outputs[5]] == 0x47
                elif matches:
                    matches = outputs == (0,) * 7
                if not matches:
                    observed_failures.append(label)
                    if variant != "original":
                        controls.append({"case": label, "expected": expected, "status": result.returncode,
                                         "output_hex": payload.hex(), "stderr": result.stderr.decode(errors="replace")})
                if variant == "original":
                    records.append({"target": target, "case": label, "expected": expected,
                                    "status": result.returncode, "output_hex": payload.hex(),
                                    "stderr": result.stderr.decode(errors="replace"), "pass": matches})
            build["scenario_mismatches"] = observed_failures
            if variant != "original":
                build["controls"] = controls
            if variant == "original" and observed_failures:
                failed.append(f"scenario:{target}:{observed_failures}")
            if variant != "original" and not observed_failures:
                failed.append(f"surviving-mutant:{target}:{variant}")
            if variant == "original" and args.captured_acpi:
                paths = [p for p in sorted(args.captured_acpi.glob("*.BIN"))
                         if p.name.startswith(("DSDT", "SSDT"))]
                tables = [p.read_bytes() for p in paths]
                # Actual capture is a replay input, never a synthesized positive fixture.
                result = checked_run(runner + [str(binary)], input=frame(tables))
                records.append({"target": target, "case": "captured-table-set",
                                "tables": len(tables), "status": result.returncode,
                                "inputs": {p.name: digest(p.read_bytes()) for p in paths},
                                "output_hex": result.stdout.hex(), "stderr": result.stderr.decode(errors="replace"),
                                "claim": "offline replay only; unsupported is not hardware absence"})
                if result.returncode != UNSUPPORTED or result.stdout != b"\0" * 56 + b"\x04":
                    failed.append(f"unexpected-capture-result:{target}")
        # Entire existing action/lease/refusal scenario, not a narrowed parser-only test.
        source = args.out / f"{target}-protocol.om"
        binary = args.out / f"{target}-protocol"
        source.write_text(prefix + units["protocol.om"] + "\n" + units["diagnostic.om"])
        result = checked_run([str(compiler)], input=source.read_bytes())
        record = {"target": target, "variant": "full-protocol", "compile_status": result.returncode,
                  "source_sha256": digest(source.read_bytes()), "stderr": result.stderr.decode(errors="replace")}
        builds.append(record)
        if result.returncode:
            failed.append(f"protocol-compile:{target}")
            continue
        binary.write_bytes(result.stdout)
        binary.chmod(0o700)
        record["binary_sha256"] = digest(result.stdout)
        result = checked_run(runner + [str(binary)])
        record.update(status=result.returncode, stdout=result.stdout.decode(errors="replace"))
        if result.returncode or result.stdout != b"SMC protocol fixtures PASS; physical NOT_RUN\n":
            failed.append(f"full-protocol:{target}")
    receipt = {"scope": "HOST_SYNTHETIC and QEMU user-mode; physical NOT_RUN",
               "compiler_sha256": PIN, "platform": platform.platform(),
               "compiler_source_revision": "d9144676c5938f877a1d89f0e246842a760c62a7",
               "checker_sha256": digest(pathlib.Path(__file__).read_bytes()),
               "python": sys.version, "qemu": checked_run([qemu, "--version"]).stdout.decode(),
               "command": sys.argv, "units": {p: digest(s.encode()) for p, s in units.items()},
               "builds": builds, "cases": records, "failures": failed}
    (args.out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("SMC static discovery " + ("FAIL" if failed else "PASS") + "; physical NOT_RUN")
    for failure in failed:
        print(failure)
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
