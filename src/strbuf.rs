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

use std::ffi::CStr;
use std::io::Write;
use std::os::raw::{c_char, c_void};

/// A buffer capable of holding arbitrary data.
///
/// This buffer guarantees that its data is always terminated with a NUL for compatibility with C,
/// but it permits internal NUL values and preserves them.
#[repr(C)]
pub struct Strbuf {
    alloc: usize,
    len: usize,
    buf: *mut c_char,
}

impl Strbuf {
    /// Create a new `Strbuf`.
    ///
    /// No memory is allocated in this case.
    pub fn new() -> Self {
        Self::with_capacity(0)
    }

    /// Create a new `Strbuf` with the given capacity.
    ///
    /// If the given capacity is 0, then no memory is allocated.
    pub fn with_capacity(size: usize) -> Self {
        let mut buf = Strbuf {
            alloc: 0,
            len: 0,
            buf: std::ptr::null_mut(),
        };
        unsafe { c::strbuf_init(&mut buf as *mut Strbuf, size) }
        buf
    }

    /// Extract the bytes from this buffer.
    pub fn to_bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.buf as *mut u8 as *const u8, self.len) }
    }

    /// Extract the bytes from this buffer in a mutable state.
    pub fn to_bytes_mut(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.buf as *mut u8, self.len) }
    }

    /// Extract the bytes, including the trailine NUL.
    ///
    /// It is possible for the data to contain an internal NUL.
    pub fn to_bytes_with_nul(&self) -> &[u8] {
        // We know it is safe to add 1 to len because we allocated the extra byte, so the size of
        // the full buffer including the NUL must fit in a usize.
        unsafe { std::slice::from_raw_parts(self.buf as *mut u8 as *const u8, self.len + 1) }
    }

    /// Get this data as a `&CStr`.
    ///
    /// If the data contains an internal NUL, the `CStr` is
    pub fn as_c_str(&self) -> &CStr {
        unsafe { CStr::from_ptr(self.buf) }
    }

    /// Truncate this data to the specified length.
    ///
    /// If `len` is greater than or equal to the specified size, nothing is done.
    ///
    /// This does not reallocate the memory used for the buffer.
    pub fn truncate(&mut self, len: usize) {
        if len >= self.len {
            return;
        }
        unsafe { c::strbuf_setlen(self as *mut Strbuf, len) }
    }
}

impl Default for Strbuf {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Strbuf {
    fn drop(&mut self) {
        unsafe { c::strbuf_release(self as *mut Strbuf) }
    }
}

impl Write for Strbuf {
    fn write(&mut self, data: &[u8]) -> Result<usize, std::io::Error> {
        unsafe {
            c::strbuf_add(
                self as *mut Strbuf,
                data.as_ptr() as *const c_void,
                data.len(),
            )
        };
        Ok(data.len())
    }

    fn flush(&mut self) -> Result<(), std::io::Error> {
        Ok(())
    }
}

pub mod c {
    use super::Strbuf;
    use std::os::raw::c_void;

    extern "C" {
        pub fn strbuf_init(sb: *mut Strbuf, alloc: usize);
        pub fn strbuf_add(sb: *mut Strbuf, data: *const c_void, len: usize);
        pub fn strbuf_setlen(sb: *mut Strbuf, len: usize);
        pub fn strbuf_release(sb: *mut Strbuf);
    }
}

#[cfg(test)]
mod tests {
    use super::Strbuf;
    use std::io::Write;

    #[test]
    fn writes_data_without_nul() {
        let mut buf = Strbuf::new();
        writeln!(buf, "Hello, 🌐!").unwrap();

        assert_eq!(buf.to_bytes(), b"Hello, \xf0\x9f\x8c\x90!\n");
        assert_eq!(buf.to_bytes_mut(), b"Hello, \xf0\x9f\x8c\x90!\n");
        assert_eq!(buf.to_bytes_with_nul(), b"Hello, \xf0\x9f\x8c\x90!\n\0");
        assert_eq!(buf.as_c_str().to_bytes(), b"Hello, \xf0\x9f\x8c\x90!\n");
    }

    #[test]
    fn writes_binary_data() {
        let mut buf = Strbuf::with_capacity(4);
        buf.write_all(b"Hello,\x00\xff\xfeworld!\n").unwrap();

        assert_eq!(buf.to_bytes(), b"Hello,\0\xff\xfeworld!\n");
        assert_eq!(buf.to_bytes_mut(), b"Hello,\0\xff\xfeworld!\n");
        assert_eq!(buf.to_bytes_with_nul(), b"Hello,\0\xff\xfeworld!\n\0");
        assert_eq!(buf.as_c_str().to_bytes(), b"Hello,");
    }
}
