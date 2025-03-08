#![allow(dead_code)]

use std::borrow::Cow;
use std::fmt::{self, Display};
use std::str::Utf8Error;

use crate::gettext::gettext as __;

/// An error code usable from C.
///
/// Every non-trivial error from `Error` must be representable as an error code in this enum.
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq, Ord, PartialOrd)]
pub enum ErrorCode {
    /// An invalid UTF-8 sequence was found.
    InvalidUtf8 = 1,
}

impl From<ErrorCode> for u32 {
    fn from(ec: ErrorCode) -> u32 {
        ec as u32
    }
}

/// The main error type.
#[derive(Debug)]
pub enum Error {
    /// An annotated error.
    ///
    /// Contains a descriptive string, and the boxed error.
    Annotated(Cow<'static, str>, Box<Error>),
    /// A string was required to be UTF-8, but was not.
    InvalidUtf8(std::str::Utf8Error),
}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            // TRANSLATORS: This is a string for annotated errors.  The annotation (more
            // descriptive error message) goes first, and then the normal error.
            Self::Annotated(msg, e) => write!(f, "{}", formatx!(__("{}: {}"), msg, e).unwrap()),
            Self::InvalidUtf8(e) => write!(
                f,
                "{}",
                formatx!(__("invalid UTF-8 after byte {}"), e.valid_up_to()).unwrap()
            ),
        }
    }
}

impl From<Utf8Error> for Error {
    fn from(e: Utf8Error) -> Error {
        Error::InvalidUtf8(e)
    }
}

impl Error {
    pub fn code(&self) -> ErrorCode {
        match self {
            Self::InvalidUtf8(..) => ErrorCode::InvalidUtf8,
            Self::Annotated(_, e) => e.code(),
        }
    }
}
