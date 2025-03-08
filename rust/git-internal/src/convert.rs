/// Conversion between bytes and OS-specific strings.
///
/// Git serializes all of its paths as byte strings.  However, Windows uses potentially ill-formed
/// UTF-16 for its strings.  That is, it encodes a superset of UTF-16 that allows for unpaired
/// surrogate code points, which are not valid in UTF-16.  This encoding is commonly called WTF-16.
/// We need some way to convert these strings back and forth such that we can take a byte string
/// and turn it into something Rust will accept as a path.  Forcing UTF-8 strings would work for
/// this, but it would be unacceptable on Unix, so we don't do that here.  Instead, we encode the
/// WTF-16 into an encoding called WTF-8[^1], which is UTF-8, except that unpaired surrogates are
/// encoded as if they were a code point.
///
/// This module provides two helper traits, `OsToBytes` and `OsFromBytes`, that can convert between
/// byte strings and `OsStr` (and friends, including `Path`).  Note that on Windows, it's required
/// that the byte strings be valid UTF-8, since that's the only way we can possibly convert them to
/// anything resembling UTF-16.  On Unix, this is not necessary, and `OsFromBytes` is always
/// successful (note, however, that the OS may still later reject the pathname for invalid UTF-8,
/// such as on macOS).
///
/// There are also some helpers that can format byte strings suitable for the `Display` trait,
/// which can only write UTF-8.  This is useful when formatting error messages for display to the
/// user.
///
/// [^1]: https://simonsapin.github.io/wtf-8/
use std::borrow::{Cow, ToOwned};
use std::ffi::{OsStr, OsString};
use std::fmt;
use std::path::{Path, PathBuf};
use std::str::Utf8Error;

/// Convert an operating-system specific string to bytes.
pub trait OsToBytes {
    /// Convert this object into bytes, either by borrowing the string when possible or by creating
    /// a new one.
    fn to_bytes(&self) -> Cow<'_, [u8]>;
}

/// Create an operating-system specific string from bytes.
pub trait OsFromBytes: ToOwned {
    /// Create a new instance of this object by borrowing the byte slice or by creating a new owned
    /// version.
    fn from_bytes(slice: &[u8]) -> Result<Cow<'_, Self>, Utf8Error>;
}

#[cfg(any(windows, test))]
fn encode_wtf8<I: Iterator<Item = u16>>(iter: I) -> Vec<u8> {
    let size_hint = match iter.size_hint() {
        (_, Some(n)) => n,
        (n, _) => n,
    };
    let mut v = Vec::with_capacity(size_hint);
    let mut iter = iter.peekable();
    loop {
        let u16cp = match iter.next() {
            Some(cp) => cp,
            None => break,
        };
        let cp: u32 = if (0xd800..=0xdbff).contains(&u16cp) {
            match iter.peek() {
                Some(0xdc00..=0xdfff) => {
                    0x10000 + ((u16cp as u32 - 0xd800) << 10)
                        | (iter.next().unwrap() as u32 - 0xdc00)
                }
                _ => u16cp as u32,
            }
        } else {
            u16cp as u32
        };
        match cp {
            n @ 0x00000..=0x0007f => v.push(n as u8),
            n @ 0x00080..=0x007ff => {
                v.push(0xc0 | ((n >> 6) as u8));
                v.push(0x80 | ((n & 0x3f) as u8));
            }
            n @ 0x00800..=0x0ffff => {
                v.push(0xe0 | ((n >> 12) as u8));
                v.push(0x80 | (((n >> 6) & 0x3f) as u8));
                v.push(0x80 | ((n & 0x3f) as u8));
            }
            n @ 0x10000..=0x10ffff => {
                v.push(0xf0 | ((n >> 18) as u8));
                v.push(0x80 | (((n >> 12) & 0x3f) as u8));
                v.push(0x80 | (((n >> 6) & 0x3f) as u8));
                v.push(0x80 | ((n & 0x3f) as u8));
            }
            _ => unreachable!(),
        }
    }
    v
}

impl OsToBytes for OsStr {
    #[cfg(unix)]
    fn to_bytes(&self) -> Cow<'_, [u8]> {
        use std::os::unix::ffi::OsStrExt;
        Cow::Borrowed(self.as_bytes())
    }

    #[cfg(windows)]
    fn to_bytes(&self) -> Cow<'_, [u8]> {
        use std::os::windows::ffi::OsStrExt;
        Cow::Owned(encode_to_wtf8(self.encode_wide()))
    }
}

impl OsToBytes for OsString {
    fn to_bytes(&self) -> Cow<'_, [u8]> {
        self.as_os_str().to_bytes()
    }
}

impl OsToBytes for Path {
    fn to_bytes(&self) -> Cow<'_, [u8]> {
        self.as_os_str().to_bytes()
    }
}

impl OsToBytes for PathBuf {
    fn to_bytes(&self) -> Cow<'_, [u8]> {
        self.as_os_str().to_bytes()
    }
}

impl OsFromBytes for OsStr {
    #[cfg(unix)]
    fn from_bytes(slice: &[u8]) -> Result<Cow<'_, Self>, Utf8Error> {
        use std::os::unix::ffi::OsStrExt;
        Ok(Cow::Borrowed(OsStrExt::from_bytes(slice)))
    }

    #[cfg(windows)]
    fn from_bytes(slice: &[u8]) -> Result<Cow<'_, Self>, Utf8Error> {
        use std::os::unix::ffi::OsStringExt;
        let v = std::slice::from_utf8(s)?.encode_utf16().collect::<Vec<_>>();
        Ok(Cow::Owned(OsString::from_wide(&v)))
    }
}

impl OsFromBytes for Path {
    fn from_bytes(slice: &[u8]) -> Result<Cow<'_, Self>, Utf8Error> {
        match OsStr::from_bytes(slice)? {
            Cow::Borrowed(os) => Ok(Cow::Borrowed(Path::new(os))),
            Cow::Owned(oss) => Ok(Cow::Owned(PathBuf::from(oss))),
        }
    }
}

/// A trait to make byte strings displayable with `Display`
///
/// By default, byte slices cannot be formatted with `Display`.  This trait is designed to allow
/// converting byte slice into something that _can_ be formatted with `Display`, like so:
///
/// ```
/// use crate::convert::AsDisplayStr;
///
/// let s = format!("I am formatting some {}", b"bytes".as_display_str());
/// assert_eq!(s, "I am formatting some bytes");
///
/// let s = format!("I am formatting some {}", b"non-Unicode bytes (\xfe\xff)".as_display_str());
/// assert_eq!(s, "I am formatting some non-Unicode bytes (\\xfe\\xff)");
/// ```
#[allow(dead_code)]
pub trait AsDisplayStr<'a> {
    fn as_display_str(&'a self) -> DisplayStr<'a>;
}

impl<'a> AsDisplayStr<'a> for &'a [u8] {
    fn as_display_str(&'a self) -> DisplayStr<'a> {
        match std::str::from_utf8(self) {
            Ok(s) => DisplayStr::Str(s),
            _ => DisplayStr::Bytes(self),
        }
    }
}

/// A borrowed sequence of bytes that can be formatted with `Display` or `Debug`.
pub enum DisplayStr<'a> {
    Bytes(&'a [u8]),
    Str(&'a str),
}

impl<'a> fmt::Display for DisplayStr<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
        use std::fmt::Write;
        match self {
            Self::Bytes(arr) => {
                for c in arr
                    .iter()
                    .flat_map(|c| std::ascii::escape_default(*c))
                    .map(|c| c as char)
                {
                    f.write_char(c)?
                }
            }
            Self::Str(s) => write!(f, "{}", s)?,
        }
        Ok(())
    }
}

impl<'a> fmt::Debug for DisplayStr<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
        fmt::Display::fmt(self, f)
    }
}

#[cfg(test)]
mod tests {
    use super::encode_wtf8;
    use super::{OsFromBytes, OsToBytes};
    use std::borrow::Cow;
    use std::ffi::OsStr;

    #[test]
    fn os_str_conversions() {
        let strs = &[
            "Hello, world!",
            "\u{1f63b}", // Smiling cat face with heart-shaped eyes
        ];
        for s in strs {
            let os: &OsStr = s.as_ref();
            assert_eq!(
                s.as_bytes(),
                os.to_bytes().as_ref(),
                "{} works with to_bytes",
                s
            );

            let oss: Cow<'_, OsStr> = OsStr::from_bytes(s.as_bytes()).unwrap();
            assert_eq!(oss, os, "{} works with from_bytes", s);
        }
    }

    #[test]
    fn encode_to_wtf8() {
        let strs = &[
            "Hello, world!",
            "fatal: Se necesitó una revisión singular",
            "\u{1f63b}", // Smiling cat face with heart-shaped eyes
        ];
        for s in strs {
            let v = encode_wtf8(s.encode_utf16());
            assert_eq!(s.as_bytes(), v);
        }
    }
}
