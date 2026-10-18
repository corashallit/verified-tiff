//! Compile the production C shim, its generated stubs and the harness with
//! clang's sanitizer and libFuzzer coverage instrumentation. With the
//! `reference` feature, also build the harness against a renamed copy of the
//! pinned libtiff for the differential target.
use std::{env, fs, path::{Path, PathBuf}, process::Command};
fn run(cmd: &mut Command) {
    let status = cmd.status().expect("spawn");
    assert!(status.success(), "{cmd:?} failed");
}
fn archive(lib: &Path, objects: &[PathBuf]) {
    let _ = fs::remove_file(lib);
    run(Command::new("ar").arg("crs").arg(lib).args(objects));
}
fn main() {
    let here = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = here.join("../..").canonicalize().unwrap();
    let compat = root.join("compat");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let cc = env::var("VTIFF_FUZZ_CC").unwrap_or_else(|_| "clang".into());
    let flags = [
        "-std=c11", "-O1", "-g", "-fPIC", "-fno-omit-frame-pointer",
        "-fsanitize=address,fuzzer-no-link",
        "-fsanitize=undefined", "-fsanitize-trap=undefined",
    ];
    // Every standard tag number below the pseudo-tag range, from the pinned header.
    let header = fs::read_to_string(compat.join("include/tiff.h")).unwrap();
    let mut numbers = Vec::new();
    for line in header.lines() {
        let mut words = line.split_whitespace();
        if let (Some("#define"), Some(name), Some(value)) = (words.next(), words.next(), words.next()) {
            if ["TIFFTAG_", "EXIFTAG_", "GPSTAG_"].iter().any(|p| name.starts_with(p)) {
                if let Ok(v) = value.parse::<u32>() {
                    if v < 65536 { numbers.push(v); }
                }
            }
        }
    }
    numbers.sort();
    numbers.dedup();
    let tags: String = numbers.iter().map(|v| format!("{v},\n")).collect();
    fs::write(out.join("tags.inc"), tags).unwrap();
    let compile = |src: &Path, obj: &str, include: &Path, extra: &[&str]| {
        run(Command::new(&cc).args(flags).args(extra).arg(format!("-I{}", compat.display()))
            .arg(format!("-I{}", include.display())).arg(format!("-I{}", here.display()))
            .arg(format!("-I{}", out.display())).arg("-c").arg(src).arg("-o").arg(out.join(obj)));
        out.join(obj)
    };
    let include = compat.join("include");
    let shim = compile(&compat.join("tiff_compat.c"), "tiff_compat.o", &include, &[]);
    run(Command::new("python3").arg(compat.join("generate_stubs.py")).arg(&out).arg(&cc));
    let stubs = compile(&out.join("stubs.c"), "stubs.o", &include, &[]);
    let harness = compile(&here.join("harness.c"), "harness.o", &include, &[]);
    archive(&out.join("libvtiff_fuzz_c.a"), &[shim, stubs, harness]);
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=vtiff_fuzz_c");
    println!("cargo:rustc-link-lib=dylib=m");
    if env::var_os("CARGO_FEATURE_REFERENCE").is_some() { reference(&root, &here, &out, &compile); }
    for f in ["tiff_compat.c", "bridge.h", "generate_stubs.py", "include/tiff.h", "include/tiffio.h",
              "fields.inc", "reader_api.inc", "color_api.inc", "logluv_api.inc",
              "uv_table.inc", "writer.inc"] {
        println!("cargo:rerun-if-changed={}", compat.join(f).display());
    }
    for f in ["harness.c", "diff.c", "transcript.h"] {
        println!("cargo:rerun-if-changed={}", here.join(f).display());
    }
    println!("cargo:rerun-if-env-changed=VTIFF_FUZZ_REFERENCE");
}
/// Rename every global symbol of the reference libtiff.a, compile the harness
/// against the renamed API and link both, plus libtiff's own codec libraries,
/// into the differential binary only.
fn reference(root: &Path, here: &Path, out: &Path, compile: &dyn Fn(&Path, &str, &Path, &[&str]) -> PathBuf) {
    let source = env::var_os("VTIFF_FUZZ_REFERENCE").map(PathBuf::from)
        .unwrap_or_else(|| root.join(".tools/lerc-tiff-source/tiff-4.7.1"));
    let libtiff = source.join("libtiff");
    let nm = Command::new("nm").args(["--defined-only", "-g"]).arg(libtiff.join(".libs/libtiff.a"))
        .output().expect("nm");
    let mut names: Vec<String> = String::from_utf8(nm.stdout).unwrap().lines()
        .filter_map(|l| { let w: Vec<&str> = l.split_whitespace().collect(); (w.len() == 3).then(|| w[2].to_string()) })
        .collect();
    names.sort();
    names.dedup();
    let map: String = names.iter().map(|n| format!("{n} vtiff_reference_{n}\n")).collect();
    fs::write(out.join("reference.map"), map).unwrap();
    let defines: String = names.iter().map(|n| format!("#define {n} vtiff_reference_{n}\n")).collect();
    fs::write(out.join("reference_names.h"), defines).unwrap();
    let renamed = out.join("libvtiff_reference.a");
    run(Command::new("objcopy").arg(format!("--redefine-syms={}", out.join("reference.map").display()))
        .arg(libtiff.join(".libs/libtiff.a")).arg(&renamed));
    let harness = compile(&here.join("harness.c"), "harness_reference.o", &libtiff, &["-DVTIFF_REFERENCE"]);
    let diff = compile(&here.join("diff.c"), "diff.o", &libtiff, &[]);
    archive(&out.join("libvtiff_fuzz_diff.a"), &[harness, diff]);
    let bin = "cargo:rustc-link-arg-bin=capi_diff";
    println!("{bin}={}", out.join("libvtiff_fuzz_diff.a").display());
    println!("{bin}={}", renamed.display());
    // libtiff's configured codec dependencies, from its libtool archive.
    let la = fs::read_to_string(libtiff.join("libtiff.la")).unwrap();
    let deps = la.lines().find_map(|l| l.strip_prefix("dependency_libs='")).unwrap().trim_end_matches('\'');
    for word in deps.split_whitespace() {
        if let Some(dir) = word.strip_prefix("-L") {
            println!("{bin}=-L{dir}");
            println!("{bin}=-Wl,-rpath,{dir}");
        } else if let Some(stem) = word.strip_suffix(".la") {
            println!("{bin}={stem}.a");
        } else {
            println!("{bin}={word}");
        }
    }
    println!("cargo:rerun-if-changed={}", libtiff.join(".libs/libtiff.a").display());
}
