// allen_tokens.rs -- Allen's look as constants.
// Generated from docs/design/DESIGN.md by allen-design-tokens. Do not edit; change DESIGN.md and run
//   `cargo run -p allen-design-tokens -- generate`.
// Core types only, so no_std code can include!() this file.

/// An sRGB colour.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rgb {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

impl Rgb {
    /// Packed as the kernel's CK_RGB.
    pub const fn packed(self) -> u32 {
        (self.r as u32) << 16 | (self.g as u32) << 8 | self.b as u32
    }
}

/// A CSS cubic-bezier easing curve, control points in Q16 (65536 = 1.0).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Bezier {
    pub x1: i32,
    pub y1: i32,
    pub x2: i32,
    pub y2: i32,
}

pub mod version {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
}

pub mod name {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
}

pub mod description {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
}

pub mod colors {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const PRIMARY: Rgb = Rgb { r: 236, g: 240, b: 252 };
    pub const NIGHT: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const HINT: Rgb = Rgb { r: 150, g: 160, b: 196 };
    pub const FAINT: Rgb = Rgb { r: 130, g: 140, b: 176 };
    pub const GLOW: Rgb = Rgb { r: 150, g: 170, b: 255 };
    pub const MIST: Rgb = Rgb { r: 200, g: 210, b: 255 };
    pub const INK: Rgb = Rgb { r: 64, g: 86, b: 196 };
    pub const WARM: Rgb = Rgb { r: 255, g: 212, b: 150 };
    pub const WARM_SOFT: Rgb = Rgb { r: 255, g: 222, b: 180 };
    pub const HEART: Rgb = Rgb { r: 255, g: 210, b: 170 };
    pub const HEART_CORE: Rgb = Rgb { r: 255, g: 236, b: 214 };
    pub const HALO: Rgb = Rgb { r: 60, g: 74, b: 210 };
    pub const HALO_VIOLET: Rgb = Rgb { r: 112, g: 92, b: 255 };
    pub const SKY_GLOW: Rgb = Rgb { r: 110, g: 130, b: 255 };
    pub const PANEL: Rgb = Rgb { r: 12, g: 16, b: 38 };
    pub const LIT: Rgb = Rgb { r: 59, g: 68, b: 111 };
}

pub mod typography {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const VOICE_FONTFAMILY: &str = "Quicksand";
    pub const VOICE_FONTSIZE_PX: u32 = 27;
    pub const VOICE_FONTWEIGHT: u32 = 400;
    pub const VOICE_LINEHEIGHT_PX: u32 = 40;
    pub const VOICE_LETTERSPACING_EM: f32 = 0.02;
    pub const BODY_FONTFAMILY: &str = "Quicksand";
    pub const BODY_FONTSIZE_PX: u32 = 21;
    pub const BODY_FONTWEIGHT: u32 = 500;
    pub const BODY_LINEHEIGHT_PX: u32 = 30;
    pub const BODY_LETTERSPACING_EM: f32 = 0.01;
    pub const SMALL_FONTFAMILY: &str = "Quicksand";
    pub const SMALL_FONTSIZE_PX: u32 = 16;
    pub const SMALL_FONTWEIGHT: u32 = 500;
    pub const SMALL_LINEHEIGHT_PX: u32 = 22;
    pub const SMALL_LETTERSPACING_EM: f32 = 0.02;
    pub const TITLE_FONTFAMILY: &str = "Quicksand";
    pub const TITLE_FONTSIZE_PX: u32 = 40;
    pub const TITLE_FONTWEIGHT: u32 = 300;
    pub const TITLE_LINEHEIGHT_PX: u32 = 52;
    pub const TITLE_LETTERSPACING_EM: f32 = 0.04;
}

pub mod rounded {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const SM_PX: u32 = 8;
    pub const MD_PX: u32 = 14;
    pub const PANEL_PX: u32 = 22;
    pub const PILL_PX: u32 = 999;
}

pub mod spacing {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const XS_PX: u32 = 4;
    pub const SM_PX: u32 = 8;
    pub const MD_PX: u32 = 16;
    pub const LG_PX: u32 = 24;
    pub const XL_PX: u32 = 40;
    pub const XXL_PX: u32 = 72;
    pub const BAR_WIDTH_PX: u32 = 720;
    pub const BAR_HEIGHT_PX: u32 = 54;
    pub const BAR_BOTTOM_PX: u32 = 96;
    pub const TEXT_WIDTH_PX: u32 = 900;
    pub const WORDS_BELOW_CENTRE_PX: u32 = 250;
}

pub mod components {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const PAGE_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const PAGE_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // PAGE_TYPOGRAPHY: typography.body
    pub const ALLEN_SAYS_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const ALLEN_SAYS_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // ALLEN_SAYS_TYPOGRAPHY: typography.voice
    pub const ALLEN_TITLE_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const ALLEN_TITLE_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // ALLEN_TITLE_TYPOGRAPHY: typography.title
    pub const HINT_TEXT_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const HINT_TEXT_TEXTCOLOR: Rgb = Rgb { r: 150, g: 160, b: 196 };
    // HINT_TEXT_TYPOGRAPHY: typography.small
    pub const FINE_PRINT_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const FINE_PRINT_TEXTCOLOR: Rgb = Rgb { r: 130, g: 140, b: 176 };
    // FINE_PRINT_TYPOGRAPHY: typography.small
    pub const INPUT_BAR_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const INPUT_BAR_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // INPUT_BAR_TYPOGRAPHY: typography.body
    pub const INPUT_BAR_ROUNDED_PX: u32 = 999;
    pub const INPUT_BAR_HEIGHT_PX: u32 = 54;
    pub const INPUT_BAR_WIDTH_PX: u32 = 720;
    pub const INPUT_BAR_CARET_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const INPUT_BAR_CARET_TEXTCOLOR: Rgb = Rgb { r: 150, g: 170, b: 255 };
    pub const CHOICE_BACKGROUNDCOLOR: Rgb = Rgb { r: 12, g: 16, b: 38 };
    pub const CHOICE_TEXTCOLOR: Rgb = Rgb { r: 200, g: 210, b: 255 };
    // CHOICE_TYPOGRAPHY: typography.body
    pub const CHOICE_ROUNDED_PX: u32 = 14;
    pub const CHOICE_PADDING_PX: u32 = 16;
    pub const CHOICE_LIT_BACKGROUNDCOLOR: Rgb = Rgb { r: 59, g: 68, b: 111 };
    pub const CHOICE_LIT_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // CHOICE_LIT_TYPOGRAPHY: typography.body
    pub const CHOICE_LIT_ROUNDED_PX: u32 = 14;
    pub const CHOICE_LIT_PADDING_PX: u32 = 16;
    pub const BUTTON_PRIMARY_BACKGROUNDCOLOR: Rgb = Rgb { r: 59, g: 68, b: 111 };
    pub const BUTTON_PRIMARY_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // BUTTON_PRIMARY_TYPOGRAPHY: typography.body
    pub const BUTTON_PRIMARY_ROUNDED_PX: u32 = 999;
    pub const BUTTON_PRIMARY_PADDING_PX: u32 = 14;
    pub const BUTTON_PRIMARY_HOVER_BACKGROUNDCOLOR: Rgb = Rgb { r: 59, g: 68, b: 111 };
    pub const BUTTON_PRIMARY_HOVER_TEXTCOLOR: Rgb = Rgb { r: 255, g: 222, b: 180 };
    pub const BUTTON_SECONDARY_BACKGROUNDCOLOR: Rgb = Rgb { r: 12, g: 16, b: 38 };
    pub const BUTTON_SECONDARY_TEXTCOLOR: Rgb = Rgb { r: 200, g: 210, b: 255 };
    pub const BUTTON_SECONDARY_ROUNDED_PX: u32 = 999;
    pub const BUTTON_SECONDARY_PADDING_PX: u32 = 14;
    pub const PANEL_BACKGROUNDCOLOR: Rgb = Rgb { r: 12, g: 16, b: 38 };
    pub const PANEL_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    pub const PANEL_ROUNDED_PX: u32 = 22;
    pub const PANEL_PADDING_PX: u32 = 24;
    pub const FOUND_LINE_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const FOUND_LINE_TEXTCOLOR: Rgb = Rgb { r: 236, g: 240, b: 252 };
    // FOUND_LINE_TYPOGRAPHY: typography.body
    pub const FOUND_LINE_LABEL_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const FOUND_LINE_LABEL_TEXTCOLOR: Rgb = Rgb { r: 150, g: 160, b: 196 };
    // FOUND_LINE_LABEL_TYPOGRAPHY: typography.body
    pub const STATE_WORD_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const STATE_WORD_TEXTCOLOR: Rgb = Rgb { r: 150, g: 170, b: 255 };
    // STATE_WORD_TYPOGRAPHY: typography.small
    pub const STATE_WORD_NEEDS_YOU_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const STATE_WORD_NEEDS_YOU_TEXTCOLOR: Rgb = Rgb { r: 255, g: 212, b: 150 };
    // STATE_WORD_NEEDS_YOU_TYPOGRAPHY: typography.small
    pub const FOCUS_RING_BACKGROUNDCOLOR: Rgb = Rgb { r: 6, g: 8, b: 18 };
    pub const FOCUS_RING_TEXTCOLOR: Rgb = Rgb { r: 200, g: 210, b: 255 };
    pub const ORB_HALO_BACKGROUNDCOLOR: Rgb = Rgb { r: 60, g: 74, b: 210 };
    pub const ORB_HALO_VIOLET_BACKGROUNDCOLOR: Rgb = Rgb { r: 112, g: 92, b: 255 };
    pub const ORB_HEART_BACKGROUNDCOLOR: Rgb = Rgb { r: 255, g: 210, b: 170 };
    pub const ORB_HEART_CORE_BACKGROUNDCOLOR: Rgb = Rgb { r: 255, g: 236, b: 214 };
    pub const WATER_BACKGROUNDCOLOR: Rgb = Rgb { r: 64, g: 86, b: 196 };
    pub const SKY_WASH_BACKGROUNDCOLOR: Rgb = Rgb { r: 110, g: 130, b: 255 };
}

pub mod native_type {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const VOICE_CAP_PX: u32 = 19;
    pub const BODY_CAP_PX: u32 = 15;
    pub const SMALL_CAP_PX: u32 = 11;
    pub const VOICE_WEIGHT: u32 = 1000;
    pub const BODY_WEIGHT: u32 = 1150;
    pub const SMALL_WEIGHT: u32 = 1250;
    pub const LINE_PX: u32 = 40;
    pub const WRAP_PX: u32 = 900;
}

pub mod motion {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const EASE_SMOOTH: Bezier = Bezier { x1: 21843, y1: 0, x2: 43693, y2: 65536 };
    pub const EASE_OUT: Bezier = Bezier { x1: 21843, y1: 65536, x2: 43693, y2: 65536 };
    pub const EASE_ENTER: Bezier = Bezier { x1: 14418, y1: 39977, x2: 23593, y2: 65536 };
    pub const FRAME_MS: u32 = 16;
    pub const LETTER_MS: u32 = 35;
    pub const LETTER_COOL_MS: u32 = 900;
    pub const WORD_RISE_MS: u32 = 600;
    pub const WORDS_FADE_MS: u32 = 450;
    pub const READ_BASE_MS: u32 = 6000;
    pub const READ_PER_LETTER_MS: u32 = 70;
    pub const STATE_BLEND_MS: u32 = 700;
    pub const CHOICE_LIT_MS: u32 = 100;
    pub const DAWN_MS: u32 = 900;
    pub const DUSK_MS: u32 = 4000;
    pub const DUSK_FALLBACK_MS: u32 = 1400;
    pub const BAR_REST_MS: u32 = 9000;
    pub const BAR_RISE_MS: u32 = 900;
    pub const CARET_BREATH_MS: u32 = 1100;
    pub const CARET_HOLD_MS: u32 = 500;
    pub const MIND_FORM_MS: u32 = 3000;
    pub const ENTER_RISE_PX: u32 = 10;
    pub const ENTER_BLUR_PX: u32 = 3;
}

pub mod heartbeat {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const BREATH_PERIOD_MS: u32 = 5500;
    pub const BREATH_DEPTH: f32 = 0.03;
    pub const BREATH_DEPTH_Q16: i32 = 1966;
    pub const THINK_PULSE_PERIOD_MS: u32 = 900;
    pub const HEART_IDLE: f32 = 0.195;
    pub const HEART_IDLE_Q16: i32 = 12780;
    pub const HEART_LISTEN: f32 = 0.273;
    pub const HEART_LISTEN_Q16: i32 = 17891;
    pub const HEART_THINK: f32 = 0.469;
    pub const HEART_THINK_Q16: i32 = 30736;
    pub const HEART_SPEAK: f32 = 0.313;
    pub const HEART_SPEAK_Q16: i32 = 20513;
    pub const HEART_EASE_SPEAK_MS: u32 = 80;
    pub const HEART_EASE_MS: u32 = 400;
}

pub mod boot {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const HOLES_APPEAR_MS: u32 = 2600;
    pub const FADE_END_MS: u32 = 6800;
    pub const SPIRAL_MS: u32 = 6000;
    pub const MERGE_MS: u32 = 12000;
    pub const RIPPLE_RUN_MS: u32 = 3600;
    pub const GONE_MS: u32 = 15200;
    pub const ALLEN_BORN_MS: u32 = 12300;
    pub const ALLEN_FORMED_MS: u32 = 15300;
    pub const CENTRE_X: f32 = 0.5;
    pub const CENTRE_X_Q16: i32 = 32768;
    pub const CENTRE_Y: f32 = 0.4;
    pub const CENTRE_Y_Q16: i32 = 26214;
}

pub mod sound {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const CHIRP_RATE_HZ: u32 = 48000;
    pub const CHIRP_FROM_HZ: u32 = 200;
    pub const CHIRP_TO_HZ: u32 = 1000;
    pub const CHIRP_PEAK: f32 = 0.7;
    pub const CHIRP_PEAK_Q16: i32 = 45875;
    pub const CHIRP_IN_MS: u32 = 40;
    pub const CHIRP_RISE_MS: u32 = 600;
    pub const CHIRP_START_MS: u32 = 11400;
    pub const CHIRP_END_MS: u32 = 12300;
}

pub mod contrast {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const PRIMARY_ON_NIGHT: u32 = 7;
    pub const HINT_ON_NIGHT: f32 = 4.5;
    pub const HINT_ON_NIGHT_Q16: i32 = 294912;
    pub const FAINT_ON_NIGHT: f32 = 4.5;
    pub const FAINT_ON_NIGHT_Q16: i32 = 294912;
    pub const MIST_ON_PANEL: f32 = 4.5;
    pub const MIST_ON_PANEL_Q16: i32 = 294912;
    pub const PRIMARY_ON_LIT: f32 = 4.5;
    pub const PRIMARY_ON_LIT_Q16: i32 = 294912;
    pub const WARM_ON_NIGHT: f32 = 4.5;
    pub const WARM_ON_NIGHT_Q16: i32 = 294912;
    pub const GLOW_ON_NIGHT: f32 = 4.5;
    pub const GLOW_ON_NIGHT_Q16: i32 = 294912;
    pub const PRIMARY_ON_PANEL: u32 = 7;
    pub const WARM_SOFT_ON_LIT: f32 = 4.5;
    pub const WARM_SOFT_ON_LIT_Q16: i32 = 294912;
}

pub mod drawn_alpha {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const HINT: f32 = 1.0;
    pub const HINT_Q16: i32 = 65536;
    pub const FAINT: f32 = 1.0;
    pub const FAINT_Q16: i32 = 65536;
}

pub mod kernel_today_alpha {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const HINT: f32 = 0.586;
    pub const HINT_Q16: i32 = 38404;
    pub const FAINT: f32 = 0.43;
    pub const FAINT_Q16: i32 = 28180;
}

pub mod reduced_motion {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const NIGHT: &str = "still";
    pub const LETTER_MS: u32 = 0;
    pub const WORD_RISE_MS: u32 = 0;
    pub const STATE_BLEND_MS: u32 = 150;
    pub const DUSK_MS: u32 = 400;
    pub const BREATH_DEPTH: f32 = 0.0;
    pub const BREATH_DEPTH_Q16: i32 = 0;
    pub const THINK_PULSE_PERIOD_MS: u32 = 0;
}

pub mod keys {
    #[allow(unused_imports)]
    use super::{Bezier, Rgb};
    pub const NEXT: &str = "ArrowDown ArrowRight Tab";
    pub const PREVIOUS: &str = "ArrowUp ArrowLeft Shift+Tab";
    pub const CHOOSE: &str = "Enter Space";
    pub const NOT_NOW: &str = "Escape";
    pub const SPEAK: &str = "any letter";
}
