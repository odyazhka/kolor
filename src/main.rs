use std::env;
use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::process::Command;

// ── FFI к C-пикеру ────────────────────────────────────────────────────────
extern "C" {
    fn run_picker(init_hex: *const c_char, out_hex: *mut c_char) -> c_int;
}

// ── Читаем текущий цвет из polybar ────────────────────────────────────────
fn read_current_color() -> String {
    let home = env::var("HOME").unwrap_or_default();
    let path = format!("{}/.config/polybar/config.ini", home);
    if let Ok(content) = fs::read_to_string(&path) {
        for line in content.lines() {
            if let Some(pos) = line.find("%{B#") {
                let rest = &line[pos + 4..];
                if rest.len() >= 6 && rest[..6].chars().all(|c| c.is_ascii_hexdigit()) {
                    return format!("#{}", rest[..6].to_uppercase());
                }
            }
        }
    }
    "#FC00EF".to_string()
}

// ── Применяем цвет ────────────────────────────────────────────────────────
fn apply_color(hex: &str) {
    let home = env::var("HOME").unwrap_or_default();

    // Polybar
    for name in &["config.ini"] {
        let path = format!("{}/.config/polybar/{}", home, name);
        if let Ok(content) = fs::read_to_string(&path) {
            fs::write(&path, replace_polybar(&content, hex)).ok();
        }
    }

    // BSPWM
    let bspwmrc = format!("{}/.config/bspwm/bspwmrc", home);
    if let Ok(content) = fs::read_to_string(&bspwmrc) {
        if fs::write(&bspwmrc, replace_bspwm(&content, hex)).is_ok() {
            Command::new("bspc")
                .args(["config", "focused_border_color", hex])
                .status()
                .ok();
        }
    }

    // Rofi
    let rasi = format!("{}/.config/rofi/my_theme.rasi", home);
    if let Ok(content) = fs::read_to_string(&rasi) {
        fs::write(&rasi, replace_rofi(&content, hex)).ok();
    }

    // Перезапуск polybar
    if let Ok(poly) = env::var("POLY_LAUNCH") {
        if !poly.is_empty() {
            Command::new("bash").arg(&poly).spawn().ok();
        }
    }

    // Вывод для скриптов (совместимость с yad --color)
    println!("{}", hex);
}

// ── Замены в конфигах ─────────────────────────────────────────────────────
fn replace_polybar(content: &str, hex: &str) -> String {
    let mut out = String::with_capacity(content.len());
    let mut rest = content;
    while let Some(pos) = rest.find("%{B#") {
        out.push_str(&rest[..pos]);
        let after = &rest[pos + 4..];
        if after.len() >= 6 && after[..6].chars().all(|c| c.is_ascii_hexdigit()) {
            out.push_str("%{B");
            out.push_str(hex);
            rest = &rest[pos + 10..];
        } else {
            out.push_str("%{B#");
            rest = &rest[pos + 4..];
        }
    }
    out.push_str(rest);
    out
}

fn replace_bspwm(content: &str, hex: &str) -> String {
    let lines: Vec<String> = content
        .lines()
        .map(|line| {
            if line.trim_start().starts_with("bspc config focused_border_color") {
                format!("bspc config focused_border_color \"{}\"", hex)
            } else {
                line.to_string()
            }
        })
        .collect();
    let mut out = lines.join("\n");
    if content.ends_with('\n') {
        out.push('\n');
    }
    out
}

fn replace_rofi(content: &str, hex: &str) -> String {
    let lines: Vec<String> = content
        .lines()
        .map(|line| {
            let trimmed = line.trim_start();
            if trimmed.starts_with("pink:") {
                let indent = &line[..line.len() - trimmed.len()];
                let suffix = if trimmed.trim_end().ends_with(';') { ";" } else { "" };
                format!("{}pink: {}{}", indent, hex, suffix)
            } else {
                line.to_string()
            }
        })
        .collect();
    let mut out = lines.join("\n");
    if content.ends_with('\n') {
        out.push('\n');
    }
    out
}

// ── main ──────────────────────────────────────────────────────────────────
fn main() {
    let current = read_current_color();

    let init_cstr = CString::new(current.as_str()).unwrap();

    // Буфер для результата: "#RRGGBB\0" = 8 байт
    let mut out_buf: [c_char; 8] = [0; 8];

    let ok = unsafe { run_picker(init_cstr.as_ptr(), out_buf.as_mut_ptr()) };

    if ok == 1 {
        let hex = unsafe { CStr::from_ptr(out_buf.as_ptr()) }
            .to_str()
            .unwrap_or("")
            .to_string();

        if !hex.is_empty() {
            apply_color(&hex);
        }
    }
}
