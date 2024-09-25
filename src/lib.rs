pub mod bin;
pub mod text;

/// All functions written in C that might need to be called from Rust.
pub mod c {
    use std::os::raw::{c_char, c_int};

    extern "C" {
        /// The main entrypoint to C-based code.
        pub fn c_main(argc: c_int, argv: *const *mut c_char) -> c_int;
    }
}
