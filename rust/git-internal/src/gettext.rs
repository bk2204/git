#![allow(dead_code)]
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

#[cfg(test)]
mod tests {
    use super::gettext;

    #[test]
    fn translation_works() {
        let s = formatx!(gettext("Needed a single revision")).unwrap();
        assert_ne!(s, "", "translated string is not empty");
    }
}
