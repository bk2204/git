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

use std::convert::TryFrom;
use std::error::Error;
use std::fmt::{self, Display};

/// An error indicating an invalid object type.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd)]
pub struct InvalidObjectType(pub u32);

impl Display for InvalidObjectType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "invalid object type {}", self.0)
    }
}

impl Error for InvalidObjectType {}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum ObjectType {
    Commit = 1,
    Tree = 2,
    Blob = 3,
    Tag = 4,
}

impl TryFrom<u32> for ObjectType {
    type Error = InvalidObjectType;
    fn try_from(val: u32) -> Result<ObjectType, Self::Error> {
        match val {
            1 => Ok(ObjectType::Commit),
            2 => Ok(ObjectType::Tree),
            3 => Ok(ObjectType::Blob),
            4 => Ok(ObjectType::Tag),
            _ => Err(InvalidObjectType(val)),
        }
    }
}

impl TryFrom<PackObjectType> for ObjectType {
    type Error = InvalidObjectType;
    fn try_from(val: PackObjectType) -> Result<ObjectType, Self::Error> {
        ObjectType::try_from(val as u32)
    }
}

impl ObjectType {
    /// Return this type as a `&'static str`.
    ///
    /// The text returned will be suitable for part of the prefix hashed into objects.
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Commit => "commit",
            Self::Tree => "tree",
            Self::Blob => "blob",
            Self::Tag => "tag",
        }
    }
}

impl Display for ObjectType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum PackObjectType {
    Commit = 1,
    Tree = 2,
    Blob = 3,
    Tag = 4,
    OffsetDelta = 6,
    RefDelta = 7,
}

impl TryFrom<u32> for PackObjectType {
    type Error = InvalidObjectType;
    fn try_from(val: u32) -> Result<PackObjectType, Self::Error> {
        match val {
            1 => Ok(PackObjectType::Commit),
            2 => Ok(PackObjectType::Tree),
            3 => Ok(PackObjectType::Blob),
            4 => Ok(PackObjectType::Tag),
            6 => Ok(PackObjectType::OffsetDelta),
            7 => Ok(PackObjectType::RefDelta),
            _ => Err(InvalidObjectType(val)),
        }
    }
}

impl From<ObjectType> for PackObjectType {
    fn from(val: ObjectType) -> PackObjectType {
        match val {
            ObjectType::Commit => PackObjectType::Commit,
            ObjectType::Tree => PackObjectType::Tree,
            ObjectType::Blob => PackObjectType::Blob,
            ObjectType::Tag => PackObjectType::Tag,
        }
    }
}

pub mod c {
    use std::os::raw::c_char;

    extern "C" {
        pub fn type_name(kind: u32) -> *const c_char;
    }
}

#[cfg(test)]
mod tests {
    use super::{InvalidObjectType, ObjectType, PackObjectType};
    use std::convert::TryFrom;
    use std::ffi::CStr;

    fn type_name(val: u32) -> Option<&'static str> {
        let p = unsafe { super::c::type_name(val) };
        if p.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(p) }.to_str().ok()
    }

    #[test]
    fn round_trips() {
        let values = [
            (0u32, None, None, None),
            (
                1,
                Some(ObjectType::Commit),
                Some(PackObjectType::Commit),
                Some("commit"),
            ),
            (
                2,
                Some(ObjectType::Tree),
                Some(PackObjectType::Tree),
                Some("tree"),
            ),
            (
                3,
                Some(ObjectType::Blob),
                Some(PackObjectType::Blob),
                Some("blob"),
            ),
            (
                4,
                Some(ObjectType::Tag),
                Some(PackObjectType::Tag),
                Some("tag"),
            ),
            (5, None, None, None),
            (6, None, Some(PackObjectType::OffsetDelta), None),
            (7, None, Some(PackObjectType::RefDelta), None),
            (8, None, None, None),
            (0xffffffff, None, None, None),
        ];

        for (int, ot, pot, text) in values {
            let actual = ObjectType::try_from(int);
            match (actual, ot) {
                (Ok(kind), Some(ot)) => {
                    assert_eq!(kind, ot, "value {}", int);
                    assert_eq!(kind as u32, int, "value {}", int);
                    assert_eq!(kind.as_str(), text.unwrap(), "value {}", int);
                    assert_eq!(type_name(int), Some(kind.as_str()), "value {}", int);
                    assert_eq!(kind.to_string(), text.unwrap(), "value {}", int);
                    assert_eq!(pot.unwrap(), ot.into(), "value {}", int);
                }
                (Err(e), None) => {
                    assert_eq!(e, InvalidObjectType(int), "value {}", int);
                    assert_eq!(type_name(int), None, "value {}", int);
                }
                _ => panic!("value {}: {:?} != {:?}", int, actual, ot),
            }

            let actual = PackObjectType::try_from(int);
            match (actual, pot) {
                (Ok(kind), Some(pot)) => {
                    assert_eq!(kind, pot, "value {}", int);
                    assert_eq!(kind as u32, int, "value {}", int);
                    assert_eq!(ot, ObjectType::try_from(pot).ok(), "value {}", int);
                }
                (Err(e), None) => {
                    assert_eq!(e, InvalidObjectType(int), "value {}", int);
                }
                _ => panic!("value {}: {:?} != {:?}", int, actual, ot),
            }
        }
    }
}
