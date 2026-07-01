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

use crate::hash::{HashAlgorithm, ObjectID};
use std::borrow::Cow;
use std::os::raw::c_void;

/// A trait to map an object to another algorithm.
pub trait ObjectMapper {
    /// Map an object to another algorithm.
    ///
    /// `src` is mapped to the algorithm `to` if possible, in which case the function returns
    /// `Some` with the mapped object ID.  If no mapping exists, returns `None`.
    ///
    /// This function takes `&'b mut self` because our repository structure may modify itself to
    /// load packs and other files in order to read data.
    fn map_oid<'a, 'b: 'a>(
        &'b mut self,
        src: &'a ObjectID,
        to: HashAlgorithm,
    ) -> Option<Cow<'a, ObjectID>>;
}

/// An object mapper using a `struct repository`.
pub struct RepoObjectMapper {
    repo: *mut c_void,
}

impl RepoObjectMapper {
    pub fn new(repo: *mut c_void) -> Self {
        Self { repo }
    }
}

impl ObjectMapper for RepoObjectMapper {
    fn map_oid<'a, 'b: 'a>(
        &'b mut self,
        src: &'a ObjectID,
        to: HashAlgorithm,
    ) -> Option<Cow<'a, ObjectID>> {
        let mut dest = to.null_oid().clone();
        match unsafe {
            c::repo_oid_to_algop(
                self.repo,
                src as *const ObjectID,
                to.hash_algo_ptr(),
                &mut dest as *mut ObjectID,
            )
        } {
            0 => Some(Cow::Owned(dest)),
            _ => None,
        }
    }
}

pub mod c {
    use crate::hash::ObjectID;
    use std::os::raw::{c_int, c_void};

    extern "C" {
        pub fn repo_oid_to_algop(
            repo: *mut c_void,
            src: *const ObjectID,
            to: *const c_void,
            dest: *mut ObjectID,
        ) -> c_int;
    }
}
