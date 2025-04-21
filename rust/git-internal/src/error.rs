#![allow(dead_code)]

use std::borrow::Cow;
use std::fmt::{self, Display};
use std::path::PathBuf;
use std::str::Utf8Error;

use crate::config::ConfigOrigin;
use crate::gettext::gettext as __;
use crate::gettext::AsLocalizable;

/// An error code usable from C.
///
/// Every non-trivial error from `Error` must be representable as an error code in this enum.
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq, Ord, PartialOrd)]
pub enum ErrorCode {
    /// An invalid UTF-8 sequence was found.
    InvalidUtf8 = 1,
    /// An error was found while parsing configuration.
    ConfigIoError = 2,
    /// Unexpected data was found when parsing configuration.
    UnexpectedConfigData = 3,
    /// The configuration value was not valid.
    InvalidConfigValue = 4,
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
    /// An I/O error occurred when parsing configuration.
    ConfigIoError(Option<PathBuf>, ConfigOrigin, u64, std::io::Error),
    /// Unexpected data was found when parsing configuration.
    ///
    /// The string component should be a localized string stating what was expected or unexpected.
    UnexpectedConfigData(Option<String>, ConfigOrigin, u64, String),
    /// The configuration value was not valid.
    ///
    /// The first component is the key, the second is the value, and the last is a localized string
    /// stating what was expected or unexpected.
    InvalidConfigValue(Cow<'static, str>, Option<Cow<'static, str>>, String),
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
            Self::ConfigIoError(Some(path), _, line, e) => write!(
                f,
                "{}",
                formatx!(
                    __("failed to parse {} (line {}) as config file: {}"),
                    path.display(),
                    line,
                    e.as_localizable()
                )
                .unwrap(),
            ),
            Self::ConfigIoError(None, origin, line, e) => write!(
                f,
                "{}",
                formatx!(
                    __("failed to parse {} (line {}) as config file: {}"),
                    origin,
                    line,
                    e.as_localizable()
                )
                .unwrap(),
            ),
            Self::UnexpectedConfigData(Some(name), ConfigOrigin::Blob, line, msg) => write!(
                f,
                "{}",
                formatx!(__("bad config line {} in blob {}: {}"), line, name, msg).unwrap(),
            ),
            Self::UnexpectedConfigData(_, ConfigOrigin::Stdin, line, msg) => write!(
                f,
                "{}",
                formatx!(__("bad config line {} in standard input: {}"), line, msg).unwrap(),
            ),
            Self::UnexpectedConfigData(Some(name), ConfigOrigin::SubmoduleBlob, line, msg) => {
                write!(
                    f,
                    "{}",
                    formatx!(
                        __("bad config line {} in submodule blob {}: {}"),
                        line,
                        name,
                        msg
                    )
                    .unwrap(),
                )
            }
            Self::UnexpectedConfigData(Some(name), ConfigOrigin::File, line, msg) => write!(
                f,
                "{}",
                formatx!(__("bad config line {} in file {}: {}"), line, name, msg).unwrap(),
            ),
            Self::UnexpectedConfigData(Some(name), ConfigOrigin::Cmdline, line, msg) => write!(
                f,
                "{}",
                formatx!(
                    __("bad config line {} in command line {}: {}"),
                    line,
                    name,
                    msg
                )
                .unwrap(),
            ),
            Self::UnexpectedConfigData(Some(name), _, line, msg) => write!(
                f,
                "{}",
                formatx!(__("bad config line {} in {}: {}"), line, name, msg).unwrap(),
            ),
            Self::UnexpectedConfigData(None, _, line, msg) => write!(
                f,
                "{}",
                formatx!(__("bad config line {}: {}"), line, msg).unwrap(),
            ),
            Self::InvalidConfigValue(key, Some(value), msg) => write!(
                f,
                "{}",
                formatx!(__("invalid config value for {} ({}): {}"), key, msg, value).unwrap(),
            ),
            Self::InvalidConfigValue(key, None, msg) => write!(
                f,
                "{}",
                formatx!(__("invalid missing config value for {}: {}}"), key, msg).unwrap(),
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
            Self::ConfigIoError(..) => ErrorCode::ConfigIoError,
            Self::UnexpectedConfigData(..) => ErrorCode::UnexpectedConfigData,
            Self::InvalidConfigValue(..) => ErrorCode::InvalidConfigValue,
            Self::Annotated(_, e) => e.code(),
        }
    }
}
