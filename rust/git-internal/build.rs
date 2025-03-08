extern crate cbindgen;

use std::env;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();

    if env::var("SKIP_BINDINGS").is_err() {
        cbindgen::generate(crate_dir)
            .expect("Unable to generate bindings")
            .write_to_file("bindings.h");
    }
}
