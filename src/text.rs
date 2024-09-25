use std::ffi::OsString;

pub trait OwnedByteConversion {
    /// Convert this data into a `Vec` of bytes.
    fn into_vec(self) -> Vec<u8>;

    /// Convert a `Vec` of bytes into an instance of this string.
    fn from_vec(vec: Vec<u8>) -> Self;
}

impl OwnedByteConversion for OsString {
    #[cfg(unix)]
    fn into_vec(self) -> Vec<u8> {
        std::os::unix::ffi::OsStringExt::into_vec(self)
    }

    #[cfg(unix)]
    fn from_vec(vec: Vec<u8>) -> Self {
        std::os::unix::ffi::OsStringExt::from_vec(vec)
    }
}
