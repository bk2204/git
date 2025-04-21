#![allow(dead_code)]
#![allow(unused_imports)]

use std::ffi::CStr;
use std::fmt;
use std::io;

use crate::convert::AsDisplayStr;

#[cfg(not(feature = "gettext"))]
pub use stubs::*;

#[cfg(feature = "gettext")]
pub use gettextrs::{gettext, ngettext};

#[cfg(not(feature = "gettext"))]
mod stubs {
    pub fn gettext<T: Into<String>>(msgid: T) -> String {
        msgid.into()
    }

    pub fn ngettext<T, S>(msgid: T, msgid_plural: S, n: u32) -> String
    where
        T: Into<String>,
        S: Into<String>,
    {
        if n == 1 {
            msgid.into()
        } else {
            msgid_plural.into()
        }
    }
}

pub trait AsLocalizable<'a> {
    fn as_localizable(&'a self) -> Localizable<'a>;
}

impl<'a> AsLocalizable<'a> for &'a io::Error {
    fn as_localizable(&'a self) -> Localizable<'a> {
        Localizable::IoError(self)
    }
}

pub enum Localizable<'a> {
    IoError(&'a io::Error),
}

impl<'a> fmt::Display for Localizable<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
        match self {
            Self::IoError(e) => match e.raw_os_error() {
                Some(errno) => {
                    let err = unsafe { CStr::from_ptr(libc::strerror(errno)) };
                    write!(f, "{}", err.to_bytes().as_display_str())
                }
                None => write!(f, "{}", e),
            },
        }
    }
}

impl<'a> fmt::Debug for Localizable<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
        fmt::Display::fmt(self, f)
    }
}

#[cfg(test)]
mod tests {
    use super::gettext;

    #[test]
    fn translation_works() {
        let s = formatx!(gettext("Needed a single revision")).unwrap();
        assert_ne!(s, "", "translated string is not empty");
    }
}
