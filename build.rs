use std::env;
use std::path::PathBuf;

fn build_from_str(static_lib: &str, srcs: &str) {
    let src_files: Vec<_> = srcs.split_ascii_whitespace().collect();
    let mut cc = cc::Build::new();
    cc.shell_escaped_flags(true);
    let _ = cc.try_flags_from_environment("GIT_CFLAGS");
    for file in src_files {
        cc.file(file);
        println!("cargo:rerun-if-changed={}", file);
    }
    cc.compile(static_lib);
}

fn build_from_env(static_lib: &str, env: &str) {
    let srcs = match env::var(env) {
        Ok(srcs) => srcs,
        Err(_) => panic!("{} is missing", env),
    };
    build_from_str(static_lib, &srcs);
}

fn main() {
    // Common libraries..
    build_from_env("git-builtin", "BUILTIN_SRCS");
    build_from_str("git-common-main", "common-main.c");

    // Binary-specific libraries.
    build_from_str("git-main", "git.c");
    println!("cargo:rustc-link-arg-bin=git=-lgit-main");

    build_from_str("scalar", "scalar.c");
    println!("cargo:rustc-link-arg-bin=scalar=-lscalar");

    let programs: &[(&str, bool, &str, &[&str])] = &[
        ("daemon", true, "", &[]),
        ("http-backend", true, "", &[]),
        ("imap-send", true, "", &[]),
        ("sh-i18n--envsubst", true, "", &[]),
        ("shell", true, "", &[]),
        (
            "http-fetch",
            true,
            "http.c http-walker.c http-fetch.c",
            &["curl"],
        ),
        ("http-push", true, "http.c http-push.c", &["curl", "expat"]),
        (
            "remote-http",
            false,
            "remote-curl.c http.c http-walker.c",
            &["curl", "expat"],
        ),
    ];
    for (prog, main_c, extra_sources, libs) in programs {
        let sources = if *main_c {
            format!("{}.c {}", prog, extra_sources)
        } else {
            extra_sources.to_string()
        };
        build_from_str(&format!("git-{}", prog), &sources);
        println!("cargo:rustc-link-arg-bin=git-{}=-lgit-{}", prog, prog);
        for lib in *libs {
            println!("cargo:rustc-link-arg-bin=git-{}=-l{}", prog, lib);
        }
    }

    println!("cargo:rustc-link-lib=git-builtin");
    println!("cargo:rustc-link-lib=git-common-main");
    println!(
        "cargo:rustc-link-search={}",
        std::env::var("CARGO_MANIFEST_DIR").expect("cargo should have set CARGO_MANIFEST_DIR")
    );
    println!("cargo:rustc-link-lib=git");
    for lib in env::var("RUST_LIBS")
        .expect("need RUST_LIBS")
        .split_ascii_whitespace()
    {
        let path = PathBuf::from(lib);
        let parent = path.parent().and_then(|p| {
            if p.as_os_str().is_empty() {
                None
            } else {
                Some(p)
            }
        });
        let file = match path.file_name().and_then(|p| p.to_str()) {
            Some(file) => file,
            None => continue,
        };
        match file.strip_suffix(".a").and_then(|f| f.strip_prefix("lib")) {
            Some(lib) => {
                // We have a static library.  Provide the directory, if one, and the library name.
                if let Some(parent) = parent {
                    println!("cargo:rustc-link-search={}", parent.display());
                }
                println!("cargo:rustc-link-lib=static={}", lib);
            }
            None => println!("cargo:rustc-link-lib={}", lib),
        }
    }
}
