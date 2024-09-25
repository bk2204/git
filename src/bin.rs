use crate::c;
use crate::text::OwnedByteConversion;
use std::env;
use std::ffi::CString;
use std::os::raw::{c_char, c_int};

pub fn generic_main() {
    let args_cstr: Vec<_> = env::args_os()
        .map(|arg| CString::new(arg.into_vec()).expect("no NUL bytes in OsString"))
        .collect();
    let args_cptr: Vec<_> = args_cstr
        .iter()
        .map(|arg| arg.as_c_str().as_ptr() as *mut c_char)
        .collect();
    unsafe { c::c_main(args_cptr.len() as c_int, args_cptr.as_ptr()) };
}
