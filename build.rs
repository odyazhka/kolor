fn main() {
    cc::Build::new()
        .file("src/picker.c")
        .flag("-O2")
        .flag("-lm")
        .compile("picker");

    println!("cargo:rustc-link-lib=X11");
    println!("cargo:rustc-link-lib=m");
}
