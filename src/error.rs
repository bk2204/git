// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation: version 2 of the License, dated June 1991.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, see <https://www.gnu.org/licenses/>.

use crate::hash::InvalidHashAlgorithm;
use std::ffi::CStr;
use std::fmt::{Debug, Display, Formatter};
use std::os::raw::c_void;

#[derive(Copy, Clone)]
#[repr(C)]
union CErrorValue {
    hash_algo: u32,
    ptr: *mut c_void,
    unused: u64,
}

impl Debug for CErrorValue {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), std::fmt::Error> {
        write!(f, "CErrorValue({:x})", unsafe { self.unused })
    }
}

/// An error in a format understandable by C.
#[derive(Debug)]
#[repr(C)]
pub struct CError {
    code: u64,
    value: CErrorValue,
}

impl CError {
    /// This bit is set if `code` is an errno value.
    const BIT_IS_ERRNO: u64 = 0x8000_0000_0000_0000;
    /// This bit is set if `value` is a dynamically allocated pointer.
    const BIT_IS_ALLOCATED: u64 = 0x4000_0000_0000_0000;
    /// This bit is set if `value` is allocated from Rust.  `BIT_IS_ALLOCATED` must also be set.
    const BIT_IS_ALLOCATED_RUST: u64 = 0x2000_0000_0000_0000;
    const BITMASK: u64 = Self::BIT_IS_ERRNO | Self::BIT_IS_ALLOCATED | Self::BIT_IS_ALLOCATED_RUST;

    /// Create a new `CError` representing success.
    pub fn success() -> Self {
        Self {
            code: 0,
            value: CErrorValue { unused: 0 },
        }
    }

    /// Create a new `CError` with the given error value and no additional data.
    pub fn new(code: ErrorConstant) -> Self {
        Self {
            code: code as u64,
            value: CErrorValue { unused: 0 },
        }
    }

    /// Create a new `CError` representing an errno value.
    pub fn new_errno(errno: i32) -> Self {
        Self {
            code: (errno as u32) as u64 | Self::BIT_IS_ERRNO,
            value: CErrorValue { unused: 0 },
        }
    }

    /// Create a new `CError` with the given data and an additional payload that does not need to
    /// be freed.
    fn new_with_static_value(code: ErrorConstant, value: CErrorValue) -> Self {
        Self {
            code: code as u64,
            value,
        }
    }

    /// The error code for this `CError`.
    pub fn code(&self) -> u32 {
        if (self.code & Self::BIT_IS_ERRNO) != 0 {
            return ErrorConstant::Errno as u32;
        }
        self.code as u32
    }

    /// The error constant for this `CError` if the code is valid.
    pub fn constant(&self) -> Option<ErrorConstant> {
        if (self.code & Self::BIT_IS_ERRNO) != 0 {
            return Some(ErrorConstant::Errno);
        }
        ErrorConstant::from_u32(self.code())
    }

    /// Returns true if this is a successful value; false otherwise.
    pub fn is_success(&self) -> bool {
        self.code == 0
    }

    /// Returns true if this is an errno value.
    pub fn is_errno(&self) -> bool {
        (self.code & Self::BIT_IS_ERRNO) != 0
    }

    /// Returns the errno value if this is one; `None` otherwise.
    pub fn errno_value(&self) -> Option<i32> {
        if self.is_errno() {
            Some(self.code as i32)
        } else {
            None
        }
    }
}

impl Drop for CError {
    fn drop(&mut self) {
        if (self.code & Self::BIT_IS_ALLOCATED) != 0 {
            if (self.code & Self::BIT_IS_ALLOCATED_RUST) != 0 {
                let ptr = unsafe { self.value.ptr };
                if ptr.is_null() {
                    return;
                }
                match ErrorConstant::from_u32((self.code & !Self::BITMASK) as u32) {
                    _ => (),
                }
            } else {
                unsafe { c::free(self.value.ptr) }
            }
        }
    }
}

impl From<Error> for CError {
    fn from(e: Error) -> Self {
        match e {
            Error::Errno(errno) => Self::new_errno(errno),
            Error::InvalidHashAlgorithm(algo) => Self::new_with_static_value(
                ErrorConstant::InvalidHashAlgorithm,
                CErrorValue { hash_algo: algo },
            ),
        }
    }
}

/// A nonzero constant representing the kind of error.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
#[repr(u32)]
pub enum ErrorConstant {
    Errno = 1,
    InvalidHashAlgorithm = 2,
}

impl ErrorConstant {
    pub fn from_u32(code: u32) -> Option<Self> {
        match code {
            1 => Some(Self::Errno),
            2 => Some(Self::InvalidHashAlgorithm),
            _ => None,
        }
    }
}

/// An error in Git.
///
/// There must be a valid `ErrorConstant` for each kind of error.
#[derive(Clone, Eq, PartialEq, Ord, PartialOrd, Debug)]
#[non_exhaustive]
pub enum Error {
    /// An unsuccessful errno value.
    Errno(i32),
    /// An invalid hash algorithm with the specified integer value.
    InvalidHashAlgorithm(u32),
}

impl Error {
    /// Return the code for this error.
    pub fn constant(&self) -> ErrorConstant {
        match self {
            Self::Errno(..) => ErrorConstant::Errno,
            Self::InvalidHashAlgorithm(..) => ErrorConstant::InvalidHashAlgorithm,
            // Do not use `_` to match exhaustively here so that we force every new error to have a
            // valid error constant associated with it.  Additionally, when adding cases here, be
            // sure to add a test for this case farther down in `tests::error_tests` below, which
            // verifies that the C code that formats errors is also appropriately wired up.
        }
    }
}

impl Display for Error {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), std::fmt::Error> {
        let cerr = CError::from(self.clone());
        let cptr = unsafe {
            c::git_aerrf(CError {
                code: cerr.code,
                value: cerr.value,
            })
        };
        if cptr.is_null() {
            return write!(
                f,
                "unknown error formatting C error ({:?})",
                CError::from(self.clone())
            );
        }
        let cs = unsafe { CStr::from_ptr(cptr) };
        let ret = write!(f, "{}", cs.to_string_lossy());
        unsafe { c::free(cptr as *mut c_void) };
        std::mem::drop(cerr);
        ret
    }
}

impl std::error::Error for Error {}

impl From<InvalidHashAlgorithm> for Error {
    fn from(e: InvalidHashAlgorithm) -> Self {
        Self::InvalidHashAlgorithm(e.0)
    }
}

pub mod c {
    use super::CError;
    use std::os::raw::{c_char, c_void};

    extern "C" {
        pub fn free(ptr: *mut c_void);
        pub fn git_aerrf(err: CError) -> *mut c_char;
    }

    #[no_mangle]
    unsafe extern "C" fn git_err_free_1(err: *mut CError) {
        std::ptr::drop_in_place(err);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expected_size() {
        assert_eq!(std::mem::size_of::<CErrorValue>(), 8);
        assert_eq!(std::mem::size_of::<CError>(), 16);
    }

    #[test]
    fn errno_tests() {
        let err = Error::Errno(2);
        assert_eq!(err.constant(), ErrorConstant::Errno);

        let cerr = CError::from(err);
        assert_eq!(cerr.is_success(), false);
        assert_eq!(cerr.is_errno(), true);
        assert_eq!(cerr.code(), ErrorConstant::Errno as u32);
        assert_eq!(cerr.errno_value().unwrap(), 2);

        let other = CError::new_errno(2);
        assert_eq!(cerr.code(), other.code());
        assert_eq!(cerr.is_success(), other.is_success());
        assert_eq!(cerr.is_errno(), other.is_errno());
        assert_eq!(cerr.errno_value(), other.errno_value());
    }

    #[test]
    fn error_tests() {
        let testcases: &[(Error, ErrorConstant, &str)] = &[(
            Error::InvalidHashAlgorithm(9),
            ErrorConstant::InvalidHashAlgorithm,
            "invalid hash algorithm 9",
        )];
        for (err, constant, text) in testcases {
            assert_eq!(err.constant(), *constant, "{:?}", constant);
            assert_eq!(err.to_string(), *text, "{:?}", constant);
            let cerr = CError::from(err.clone());
            assert_eq!(cerr.code(), *constant as u32, "{:?}", constant);
            assert_eq!(cerr.constant().unwrap(), *constant, "{:?}", constant);
            assert_eq!(cerr.is_success(), false, "{:?}", constant);
            assert_eq!(cerr.is_errno(), false, "{:?}", constant);
            assert_eq!(cerr.errno_value(), None, "{:?}", constant);
        }
    }
}
