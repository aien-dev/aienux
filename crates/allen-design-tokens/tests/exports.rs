//! The committed exports must match DESIGN.md, every gated contrast pair must pass, and the tokens lifted
//! from the kernel must keep the kernel's values.

use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn tokens() -> allen_design_tokens::Tokens {
    let text = std::fs::read_to_string(root().join("docs/design/DESIGN.md")).unwrap();
    allen_design_tokens::parse(&text).unwrap()
}

#[test]
fn exports_are_current() {
    for (name, body) in allen_design_tokens::outputs(&tokens()).unwrap() {
        let path = root().join("docs/design/tokens").join(name);
        let on_disk = std::fs::read_to_string(&path).unwrap_or_default();
        assert!(
            on_disk == body,
            "{} is stale: run `cargo run -p allen-design-tokens -- generate`",
            path.display()
        );
    }
}

#[test]
fn gated_contrast_passes() {
    for r in allen_design_tokens::check_contrast(&tokens()).unwrap() {
        assert!(
            !r.gated || r.passes(),
            "{} on {}: {:.2}:1 below {:.1}",
            r.fg,
            r.bg,
            r.ratio,
            r.floor
        );
    }
}

#[test]
fn kernel_values_are_kept() {
    // native/kernel/ui on feat/allen-boot-everywhere (iMac 0930240): night.h, chirp.h, allen_ui.c, mind.c
    let t = tokens();
    for (path, want) in [
        ("colors.night", "#060812"),
        ("colors.primary", "#ECF0FC"),
        ("colors.hint", "#96A0C4"),
        ("colors.faint", "#828CB0"),
        ("colors.glow", "#96AAFF"),
        ("colors.mist", "#C8D2FF"),
        ("colors.warm", "#FFD496"),
        ("colors.heart", "#FFD2AA"),
        ("colors.heart-core", "#FFECD6"),
        ("colors.halo", "#3C4AD2"),
        ("colors.halo-violet", "#705CFF"),
        ("colors.sky-glow", "#6E82FF"),
        ("boot.holes-appear", "2600ms"),
        ("boot.fade-end", "6800ms"),
        ("boot.spiral", "6000ms"),
        ("boot.merge", "12000ms"),
        ("boot.ripple-run", "3600ms"),
        ("boot.gone", "15200ms"),
        ("boot.allen-born", "12300ms"),
        ("boot.allen-formed", "15300ms"),
        ("sound.chirp-rate", "48000Hz"),
        ("sound.chirp-from", "200Hz"),
        ("sound.chirp-to", "1000Hz"),
        ("sound.chirp-in", "40ms"),
        ("sound.chirp-rise", "600ms"),
        ("sound.chirp-start", "11400ms"),
        ("sound.chirp-end", "12300ms"),
        ("motion.letter", "35ms"),
        ("motion.mind-form", "3000ms"),
        ("motion.caret-breath", "1100ms"),
        ("heartbeat.breath-period", "5500ms"),
        ("heartbeat.think-pulse-period", "900ms"),
        ("native-type.voice-cap", "19px"),
        ("native-type.body-cap", "15px"),
        ("native-type.small-cap", "11px"),
        ("native-type.line", "40px"),
        ("native-type.wrap", "900px"),
        ("spacing.bar-width", "720px"),
        ("spacing.bar-height", "54px"),
    ] {
        assert_eq!(t.get(path), Some(want), "{path}");
    }
    let c = allen_design_tokens::emit_c(&t);
    // the kernel's fixed-point forms: chirp peak 22937 (CK_CHIRP_PEAK), breath 1966/65536 (mind.c 173),
    // heart levels 50, 70, 120, 80 out of 256 (mind.c 137)
    for line in [
        "#define ALLEN_SOUND_CHIRP_PEAK_Q15 22937",
        "#define ALLEN_HEARTBEAT_BREATH_DEPTH_Q16 1966",
        "#define ALLEN_HEARTBEAT_HEART_IDLE_Q8 50",
        "#define ALLEN_HEARTBEAT_HEART_LISTEN_Q8 70",
        "#define ALLEN_HEARTBEAT_HEART_THINK_Q8 120",
        "#define ALLEN_HEARTBEAT_HEART_SPEAK_Q8 80",
    ] {
        assert!(c.contains(line), "missing `{line}`");
    }
}
