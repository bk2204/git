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

use std::convert::TryInto;

pub(crate) fn element_offset<T>(data: &[T], item: &T) -> Option<usize> {
    // TODO: This is a builtin slice method starting from 1.94.0.  Remove it when we require
    // that version.
    let data = data.as_ptr();
    let item = item as *const T;
    unsafe { item.offset_from(data) }.try_into().ok()
}

#[cfg(test)]
pub(crate) mod tests {
    pub(crate) fn strip_q(data: &[u8]) -> Vec<u8> {
        data.iter()
            .cloned()
            .enumerate()
            .filter_map(|(i, b)| {
                if i + 1 >= data.len() || !(b == b'Q' && data[i + 1] == b'\n') {
                    Some(b)
                } else {
                    None
                }
            })
            .collect()
    }
}
