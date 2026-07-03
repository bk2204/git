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

use super::ObjectType;
use crate::hash::{HashAlgorithm, ObjectID};
use crate::object::header::{HeaderObjectParser, ParsedObjectComponent, ParsedObjectKind};
use crate::object::tree::ParseableTree;
use crate::signature::parse_signed_buffer;
use std::borrow::Cow;
use std::io::{Cursor, Write};
use std::num::NonZeroU32;
use std::os::raw::{c_char, c_void};

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

#[derive(Clone, Ord, PartialOrd, Eq, PartialEq, Debug)]
pub enum ConversionError {
    /// Indicates an unsuccessful mapping due to a missing object.
    ///
    /// The first component is the object type being converted.  The second component is the type
    /// that was unable to be found with the relevant missing object information.  For instance,
    /// `MissingObject(ObjectType::Commit, "tree", _)` indicates that the commit was unable to be
    /// mapped because the `tree` header contained the specified missing object.
    MissingObject(ObjectType, &'static str, MissingObjectError),
    InvalidTreeEntry,
    InvalidObject(ObjectType),
    UnknownHeader(ObjectType),
    InvalidHeader(ObjectType),
    InvalidObjectID(ObjectType, &'static str),
}

#[repr(C)]
pub struct ConversionErrorData {
    pub kind: ObjectType,
    pub header: *const c_char,
    pub header_len: usize,
    pub missing_object: MissingObjectError,
}

#[derive(Clone, Ord, PartialOrd, Eq, PartialEq, Debug)]
#[repr(C)]
pub struct MissingObjectError {
    oid: ObjectID,
    mode: Option<NonZeroU32>,
    kind: Option<NonZeroU32>,
}

impl MissingObjectError {
    pub fn new(oid: ObjectID, mode: Option<u16>, kind: Option<ObjectType>) -> Self {
        Self {
            oid,
            mode: mode.and_then(|val| NonZeroU32::new(val as u32)),
            kind: kind.and_then(|val| NonZeroU32::new(val as u32)),
        }
    }
}

pub struct ObjectConverter<'a> {
    mapper: &'a mut dyn ObjectMapper,
    from: HashAlgorithm,
    to: HashAlgorithm,
}

impl<'a> ObjectConverter<'a> {
    pub fn new(mapper: &'a mut dyn ObjectMapper, from: HashAlgorithm, to: HashAlgorithm) -> Self {
        Self { mapper, from, to }
    }

    pub fn convert<W: Write>(
        &mut self,
        kind: ObjectType,
        data: &[u8],
        writer: W,
        skip_sigs: bool,
    ) -> Result<u64, ConversionError> {
        let mut writer = writer;

        if self.from == self.to {
            return Ok(writer.write(data).unwrap_or(0usize) as u64);
        }

        match kind {
            ObjectType::Blob => Ok(writer.write(data).unwrap_or(0usize) as u64),
            ObjectType::Tree => self.convert_tree(data, writer),
            ObjectType::Commit => self.convert_commit(data, writer),
            ObjectType::Tag => self.convert_tag(data, writer, skip_sigs),
        }
    }

    fn convert_tree<W: Write>(&mut self, data: &[u8], writer: W) -> Result<u64, ConversionError> {
        let mut writer = writer;
        let mut total_written = 0u64;
        let tree = ParseableTree::new(data, self.from);
        for entry in tree.iter() {
            let entry = entry.map_err(|_| ConversionError::InvalidTreeEntry)?;
            let oid = self
                .mapper
                .map_oid(entry.object_id(), self.to)
                .ok_or_else(|| {
                    let ot = ObjectType::from_mode(entry.mode());
                    ConversionError::MissingObject(
                        ObjectType::Tree,
                        ot.as_str(),
                        MissingObjectError::new(
                            entry.object_id().clone(),
                            Some(entry.mode()),
                            Some(ot),
                        ),
                    )
                })?;
            total_written += writer.write(entry.text_mode()).unwrap_or(0) as u64;
            total_written += writer.write(b" ").unwrap_or(0) as u64;
            total_written += writer.write(entry.path()).unwrap_or(0) as u64;
            total_written += writer.write(b"\0").unwrap_or(0) as u64;
            total_written += writer.write(oid.as_slice().unwrap()).unwrap_or(0) as u64;
        }
        Ok(total_written)
    }

    fn convert_tag<W: Write>(
        &mut self,
        data: &[u8],
        writer: W,
        skip_sigs: bool,
    ) -> Result<u64, ConversionError> {
        let mut writer = writer;
        let mut othersig = None;
        let mut total_written = 0u64;
        let mut parser = HeaderObjectParser::new(data, true, true);
        match parser.next() {
            Some(Ok(ParsedObjectComponent {
                kind: ParsedObjectKind::Header(b"object", oid),
                ..
            })) => {
                total_written +=
                    self.map_oid_header(&mut writer, ObjectType::Tag, None, "object", &oid)?;
            }
            _ => return Err(ConversionError::InvalidObject(ObjectType::Tag)),
        };
        for obj in parser {
            let obj = obj.map_err(|_| ConversionError::InvalidHeader(ObjectType::Tag))?;
            let kind = obj.kind;
            match kind {
                ParsedObjectKind::Header(header, body)
                    if header == self.to.gpgsig_header().as_bytes() =>
                {
                    othersig = Some(body);
                }
                ParsedObjectKind::Header(header, _) if header.starts_with(b"gpgsig") => {
                    // We are not equipped to handle a third algorithm in this case.
                    return Err(ConversionError::InvalidObject(ObjectType::Tag));
                }
                ParsedObjectKind::Header(_, _) => {
                    total_written += writer.write(&data[obj.start..obj.end]).unwrap_or(0) as u64;
                }
                ParsedObjectKind::Body => {
                    let (body, oursig) = if skip_sigs {
                        (&data[obj.start..obj.end], None)
                    } else {
                        parse_signed_buffer(&data[obj.start..obj.end])
                    };
                    if let Some(sig) = oursig {
                        let sig = if let Some(b'\n') = sig.last() {
                            &sig[..sig.len() - 1]
                        } else {
                            sig
                        };
                        total_written += writer
                            .write(self.from.gpgsig_header().as_bytes())
                            .unwrap_or(0) as u64;
                        for line in sig.split(|&b| b == b'\n') {
                            total_written += writer.write(b" ").unwrap_or(0) as u64;
                            total_written += writer.write(line).unwrap_or(0) as u64;
                            total_written += writer.write(b"\n").unwrap_or(0) as u64;
                        }
                    }
                    total_written += writer.write(body).unwrap_or(0) as u64;
                    if let Some(sig) = othersig.take() {
                        total_written += writer.write(&sig).unwrap_or(0) as u64;
                        total_written += writer.write(b"\n").unwrap_or(0) as u64;
                    }
                }
            }
        }
        Ok(total_written)
    }

    fn map_oid_header<W: Write>(
        &mut self,
        writer: &mut W,
        objtype: ObjectType,
        mapped_type: Option<ObjectType>,
        header: &'static str,
        oid: &[u8],
    ) -> Result<u64, ConversionError> {
        let oid = ObjectID::from_hex(oid, self.from)
            .map_err(|_| ConversionError::InvalidObjectID(objtype, header))?;
        let mapped = self
            .mapper
            .map_oid(&oid, self.to)
            .ok_or(ConversionError::MissingObject(
                objtype,
                header,
                MissingObjectError::new(oid.clone(), None, mapped_type),
            ))?;
        let _ = writeln!(writer, "{} {}", header, mapped);
        Ok(header.len() as u64 + 1 + mapped.algo().unwrap().hex_len() as u64 + 1)
    }

    fn convert_commit<W: Write>(&mut self, data: &[u8], writer: W) -> Result<u64, ConversionError> {
        let mut writer = writer;
        let mut total_written = 0u64;

        let parser = HeaderObjectParser::new(data, true, true);
        for obj in parser {
            let obj = obj.map_err(|_| ConversionError::InvalidHeader(ObjectType::Commit))?;
            let kind = obj.kind;
            match kind {
                ParsedObjectKind::Header(b"tree", oid) => {
                    total_written += self.map_oid_header(
                        &mut writer,
                        ObjectType::Commit,
                        Some(ObjectType::Tree),
                        "tree",
                        &oid,
                    )?;
                }
                ParsedObjectKind::Header(b"parent", oid) => {
                    total_written += self.map_oid_header(
                        &mut writer,
                        ObjectType::Commit,
                        Some(ObjectType::Commit),
                        "parent",
                        &oid,
                    )?;
                }
                ParsedObjectKind::Header(b"mergetag", body) => {
                    let mut c = Cursor::new(Vec::new());
                    // body will not include the trailing newline, but we need that to convert
                    // signatures properly.
                    let mut body = body.into_owned();
                    body.extend(b"\n");
                    self.convert_tag(&body, &mut c, false)?;
                    let tag = c.into_inner();
                    total_written += writer.write(b"mergetag ").unwrap_or(0) as u64;
                    let mut eol = match tag.iter().enumerate().find(|&(_, b)| *b == b'\n') {
                        Some((n, _)) => {
                            total_written += writer.write(&tag[0..n + 1]).unwrap_or(0) as u64;
                            n + 1
                        }
                        None => {
                            total_written += writer.write(&tag).unwrap_or(0) as u64;
                            tag.len()
                        }
                    };
                    while let Some((n, _)) =
                        tag[eol..].iter().enumerate().find(|&(_, b)| *b == b'\n')
                    {
                        total_written += writer.write(b" ").unwrap_or(0) as u64;
                        total_written += writer.write(&tag[eol..eol + n + 1]).unwrap_or(0) as u64;
                        eol += n + 1;
                    }
                    if tag.len() > eol {
                        total_written += writer.write(b" ").unwrap_or(0) as u64;
                        total_written += writer.write(&tag[eol..]).unwrap_or(0) as u64;
                    }
                }
                ParsedObjectKind::Header(b"author", _)
                | ParsedObjectKind::Header(b"committer", _)
                | ParsedObjectKind::Header(b"encoding", _) => {
                    total_written += writer.write(&data[obj.start..obj.end]).unwrap_or(0) as u64;
                }
                ParsedObjectKind::Header(header, _) if header.starts_with(b"gpgsig") => {
                    total_written += writer.write(&data[obj.start..obj.end]).unwrap_or(0) as u64;
                }
                // Fail on unknown lines as they might contain an object ID.
                ParsedObjectKind::Header(..) => {
                    return Err(ConversionError::UnknownHeader(ObjectType::Commit))
                }
                ParsedObjectKind::Body => {
                    total_written += writer.write(&data[obj.start..obj.end]).unwrap_or(0) as u64;
                    return Ok(total_written);
                }
            }
        }
        Ok(total_written)
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::loose::{MapType, ObjectMap};
    use crate::util::tests::strip_q;
    use std::io::Cursor;

    fn default_mappings() -> ObjectMap {
        let main = HashAlgorithm::SHA1;
        let compat = HashAlgorithm::SHA256;
        let mut map = ObjectMap::new(main, compat);
        let inserter = |map: &mut ObjectMap, oid1, oid2| {
            map.insert(
                &ObjectID::from_hex(oid1, main).unwrap(),
                &ObjectID::from_hex(oid2, compat).unwrap(),
                MapType::LooseObject,
                false,
            );
        };
        inserter(
            &mut map,
            b"1111111111111111111111111111111111111111",
            b"1111111111111111111111111111111111111111111111111111111111111111",
        );
        inserter(
            &mut map,
            b"2222222222222222222222222222222222222222",
            b"2222222222222222222222222222222222222222222222222222222222222222",
        );
        inserter(
            &mut map,
            b"3333333333333333333333333333333333333333",
            b"3333333333333333333333333333333333333333333333333333333333333333",
        );
        inserter(
            &mut map,
            b"4444444444444444444444444444444444444444",
            b"4444444444444444444444444444444444444444444444444444444444444444",
        );
        inserter(
            &mut map,
            b"5555555555555555555555555555555555555555",
            b"5555555555555555555555555555555555555555555555555555555555555555",
        );
        inserter(
            &mut map,
            b"6666666666666666666666666666666666666666",
            b"6666666666666666666666666666666666666666666666666666666666666666",
        );
        inserter(
            &mut map,
            b"7777777777777777777777777777777777777777",
            b"7777777777777777777777777777777777777777777777777777777777777777",
        );
        inserter(
            &mut map,
            b"8888888888888888888888888888888888888888",
            b"8888888888888888888888888888888888888888888888888888888888888888",
        );
        inserter(
            &mut map,
            b"9999999999999999999999999999999999999999",
            b"9999999999999999999999999999999999999999999999999999999999999999",
        );
        map
    }

    #[test]
    fn converts_trees_success() {
        let mut map = default_mappings();
        let input = b"100644 .gitignore\x00\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x5540000 dir\x00\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11";
        let expected = b"100644 .gitignore\x00\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x55\x5540000 dir\x00\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11";
        let mut cvt = ObjectConverter::new(&mut map, HashAlgorithm::SHA1, HashAlgorithm::SHA256);
        let mut curs = Cursor::new(Vec::new());
        cvt.convert(ObjectType::Tree, input, &mut curs, false)
            .unwrap();
        assert_eq!(curs.into_inner(), expected);

        let mut cvt = ObjectConverter::new(&mut map, HashAlgorithm::SHA256, HashAlgorithm::SHA1);
        let mut curs = Cursor::new(Vec::new());
        cvt.convert(ObjectType::Tree, expected, &mut curs, false)
            .unwrap();
        assert_eq!(curs.into_inner(), input);
    }

    #[test]
    fn converts_tags_success() {
        let mut map = default_mappings();
        let testcases: &[(&str, &[u8], &[u8])] = &[
            (
                "double-signed tags",
                b"object 4444444444444444444444444444444444444444
type commit
tag trace-rv-v6.17-rc5
tagger C O Ntributor <contributor@example.com> 1758226661 -0400
gpgsig-sha256 -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =BBBB
 -----END PGP SIGNATURE-----

Tag message.

More lines.
-----BEGIN PGP SIGNATURE-----

iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
=CCCC
-----END PGP SIGNATURE-----
",
                b"object 4444444444444444444444444444444444444444444444444444444444444444
type commit
tag trace-rv-v6.17-rc5
tagger C O Ntributor <contributor@example.com> 1758226661 -0400
gpgsig -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =CCCC
 -----END PGP SIGNATURE-----

Tag message.

More lines.
-----BEGIN PGP SIGNATURE-----

iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
=BBBB
-----END PGP SIGNATURE-----
",
            ),
            (
                "SHA-1 signed tags",
                b"object 4444444444444444444444444444444444444444
type commit
tag trace-rv-v6.17-rc5
tagger C O Ntributor <contributor@example.com> 1758226661 -0400

Tag message.

More lines.
-----BEGIN PGP SIGNATURE-----

iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
=CCCC
-----END PGP SIGNATURE-----
",
                b"object 4444444444444444444444444444444444444444444444444444444444444444
type commit
tag trace-rv-v6.17-rc5
tagger C O Ntributor <contributor@example.com> 1758226661 -0400
gpgsig -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =CCCC
 -----END PGP SIGNATURE-----

Tag message.

More lines.
",
            ),
        ];

        for (desc, input, expected) in testcases {
            let input = strip_q(input);
            let expected = strip_q(expected);

            let mut cvt =
                ObjectConverter::new(&mut map, HashAlgorithm::SHA1, HashAlgorithm::SHA256);
            let mut curs = Cursor::new(Vec::new());
            cvt.convert(ObjectType::Tag, &input, &mut curs, false)
                .unwrap();
            assert_eq!(curs.into_inner(), expected, "{}: SHA-1 → SHA-256", desc);

            let mut cvt =
                ObjectConverter::new(&mut map, HashAlgorithm::SHA256, HashAlgorithm::SHA1);
            let mut curs = Cursor::new(Vec::new());
            cvt.convert(ObjectType::Tag, &expected, &mut curs, false)
                .unwrap();
            assert_eq!(curs.into_inner(), input, "{}: SHA-256 → SHA-1", desc);
        }
    }

    #[test]
    fn converts_commits_success() {
        let mut map = default_mappings();
        let testcases: &[(&str, &[u8], &[u8])] = &[(
            "double-signed commits with mergetag",
            b"tree 1111111111111111111111111111111111111111
parent 2222222222222222222222222222222222222222
parent 3333333333333333333333333333333333333333
author A U Thor <author@example.com> 1758234120 -0700
committer C O Mitter <committer@example.com> 1758234120 -0700
mergetag object 4444444444444444444444444444444444444444
 type commit
 tag trace-rv-v6.17-rc5
 tagger C O Ntributor <contributor@example.com> 1758226661 -0400
 Q
 Patch to fix bug.
 Q
 More lines.
 -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =CCCC
 -----END PGP SIGNATURE-----
gpgsig -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =AAAA
 -----END PGP SIGNATURE-----
gpgsig-sha256 -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =BBBB
 -----END PGP SIGNATURE-----

Commit message.

More lines.
",
            b"tree 1111111111111111111111111111111111111111111111111111111111111111
parent 2222222222222222222222222222222222222222222222222222222222222222
parent 3333333333333333333333333333333333333333333333333333333333333333
author A U Thor <author@example.com> 1758234120 -0700
committer C O Mitter <committer@example.com> 1758234120 -0700
mergetag object 4444444444444444444444444444444444444444444444444444444444444444
 type commit
 tag trace-rv-v6.17-rc5
 tagger C O Ntributor <contributor@example.com> 1758226661 -0400
 gpgsig -----BEGIN PGP SIGNATURE-----
  Q
  iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
  b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
  ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
  =CCCC
  -----END PGP SIGNATURE-----
 Q
 Patch to fix bug.
 Q
 More lines.
gpgsig -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =AAAA
 -----END PGP SIGNATURE-----
gpgsig-sha256 -----BEGIN PGP SIGNATURE-----
 Q
 iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
 b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
 ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
 =BBBB
 -----END PGP SIGNATURE-----

Commit message.

More lines.
",
        )];

        for (desc, input, expected) in testcases {
            let input = strip_q(input);
            let expected = strip_q(expected);

            let mut cvt =
                ObjectConverter::new(&mut map, HashAlgorithm::SHA1, HashAlgorithm::SHA256);
            let mut curs = Cursor::new(Vec::new());
            cvt.convert(ObjectType::Commit, &input, &mut curs, false)
                .unwrap();
            assert_eq!(curs.into_inner(), expected, "{}: SHA-1 → SHA-256", desc);

            let mut cvt =
                ObjectConverter::new(&mut map, HashAlgorithm::SHA256, HashAlgorithm::SHA1);
            let mut curs = Cursor::new(Vec::new());
            cvt.convert(ObjectType::Commit, &expected, &mut curs, false)
                .unwrap();
            assert_eq!(curs.into_inner(), input, "{}: SHA-256 → SHA-1", desc);
        }
    }
}
