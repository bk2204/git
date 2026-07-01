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

#[derive(Copy, Clone, Debug)]
pub struct InvalidTreeEntry;

pub struct ParseableTree<'a> {
    data: &'a [u8],
    algo: HashAlgorithm,
}

impl<'a> ParseableTree<'a> {
    pub fn new(data: &'a [u8], algo: HashAlgorithm) -> Self {
        Self { data, algo }
    }

    pub fn iter(&self) -> TreeEntryIter<'a> {
        TreeEntryIter {
            data: self.data,
            algo: self.algo,
            off: 0,
            finished: false,
        }
    }
}

pub struct TreeEntryIter<'a> {
    data: &'a [u8],
    algo: HashAlgorithm,
    off: usize,
    finished: bool,
}

impl<'a> Iterator for TreeEntryIter<'a> {
    type Item = Result<TreeEntry<'a>, InvalidTreeEntry>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.off == self.data.len() || self.finished {
            return None;
        }
        match TreeEntry::new(&self.data[self.off..], self.algo) {
            Ok(te) => {
                self.off += te.len();
                Some(Ok(te))
            }
            Err(e) => {
                self.finished = true;
                Some(Err(e))
            }
        }
    }
}

pub struct TreeEntry<'a> {
    entry: &'a [u8],
    path: &'a [u8],
    mode: &'a [u8],
    nmode: u16,
    oid: ObjectID,
}

impl<'a> TreeEntry<'a> {
    /// Create a new `TreeEntry` by parsing a single tree entry for `algo` from `data`.
    ///
    /// `data` may be longer than the tree entry.  The amount of data that was used for this entry
    /// may be found by calling `len`.
    pub fn new(data: &'a [u8], algo: HashAlgorithm) -> Result<Self, InvalidTreeEntry> {
        if data.len() < algo.raw_len() + 3 || data.len() == (algo.raw_len() + 1) {
            return Err(InvalidTreeEntry);
        }
        let to_parse = data;
        let (mode, nmode) = Self::parse_tree_mode(to_parse).ok_or(InvalidTreeEntry)?;
        let to_parse = &to_parse[mode.len()..];
        if to_parse.is_empty() || to_parse[0] != b' ' {
            return Err(InvalidTreeEntry);
        }
        let to_parse = &to_parse[1..];
        let idx = to_parse
            .iter()
            .cloned()
            .enumerate()
            .find(|(_, b)| *b == 0)
            .map(|(idx, _)| idx)
            .ok_or(InvalidTreeEntry)?;
        let path = &to_parse[0..idx];
        if to_parse.len() == idx {
            return Err(InvalidTreeEntry);
        }
        let to_parse = &to_parse[idx + 1..];
        if to_parse.len() < algo.raw_len() {
            return Err(InvalidTreeEntry);
        }
        let oid = ObjectID::new(algo, &to_parse[0..algo.raw_len()]);
        let to_parse = &to_parse[algo.raw_len()..];
        Ok(TreeEntry {
            entry: &data[0..data.len() - to_parse.len()],
            mode,
            nmode,
            path,
            oid,
        })
    }

    /// The number of bytes in the full tree entry.
    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> usize {
        self.entry.len()
    }

    /// The mode portion of the entry as bytes.
    pub fn text_mode(&self) -> &[u8] {
        self.mode
    }

    /// The mode portion of the entry as a numeric value.
    pub fn mode(&self) -> u16 {
        self.nmode
    }

    /// The path portion of the tree entry without a trailing NUL.
    pub fn path(&self) -> &[u8] {
        self.path
    }

    /// The object ID in the tree entry.
    pub fn object_id(&self) -> &ObjectID {
        &self.oid
    }

    fn parse_tree_mode(buf: &[u8]) -> Option<(&[u8], u16)> {
        let mut mode = 0u16;
        if buf.is_empty() || buf[0] == b' ' {
            return None;
        }
        for (i, &b) in buf.iter().enumerate() {
            if b == b' ' {
                return Some((&buf[0..i], mode));
            }
            if !(b'0'..=b'7').contains(&b) {
                return None;
            }
            mode <<= 3;
            mode |= (b - b'0') as u16;
        }
        Some((buf, mode))
    }
}

#[cfg(test)]
mod tests {
    use super::{ParseableTree, TreeEntry};
    use crate::hash::HashAlgorithm;
    use crate::object::ObjectType;
    use std::io::{Cursor, Write};

    fn write_tree_entry(w: &mut dyn Write, e: &TreeEntry<'_>) {
        write!(
            w,
            "{:06o} {} {}\t",
            e.mode(),
            ObjectType::from_mode(e.mode()).as_str(),
            e.object_id()
        )
        .unwrap();
        w.write_all(e.path()).unwrap();
        w.write_all(b"\n").unwrap();
    }

    #[test]
    fn parse_tree_mode() {
        let testcases: &[(&[u8], Option<(&[u8], u16)>)] = &[
            (b"100644 ", Some((b"100644", 0o100644))),
            (b"40000 ", Some((b"40000", 0o040000))),
            (b"040000 ", Some((b"040000", 0o040000))),
            (b"080000 ", None),
        ];

        for (input, expected) in testcases {
            assert_eq!(
                TreeEntry::parse_tree_mode(input),
                *expected,
                "{}",
                input.escape_ascii()
            );
        }
    }

    #[test]
    fn parses_sha256() {
        let data = b"100644 .gitignore\x00\xB8\x015w\xD7&\xC0\x8B^\xB0\xC0\x1F\xF5\x9F\x9EF\xFB\xE8\x04\xC3I\xB9\xF3\x91\x0A'\x3D\xAC\xA4\xB9F\x17100644 .rustfmt.toml\x00\x91NxI\x9D\xFC\x0B8?\x89\xAC\xF1\xDD\xAF$\xF2\xFE\xBA\x92\x93\xE0\x8A\x88;\x88$}\x0C\xEBJ\xE8\x82100644 CHANGELOG.adoc\x00\x98\xABI\xBCPm$\xEB\x16\xE4D\x9C\x08n\xAD\xDC\xD9\x08\x98?\x99\x9F\x1F\x96\xC2\x15e\xB1 Q\x17`100644 Cargo.lock\x00\x9E(\xAAp\x1A\xF9\xB9e8oY\xA9H\xF5[}\x9F\x88\x1Am>\xD1p\xF4#\xC6a>T\xAAh3100644 Cargo.toml\x00\xA2\x99\xE6\x9D\xE0-\x1E\xFBb\x8F\x9D\xBFn\x84\x01\xCApD\xE9x\xBC*9<\x8C`\xC4\x1A\xF2i41100644 LICENSE\x00\x88%0\x12\xD1\xD8\x8E\xB2\xE3Pe\x8B\x9B\x0E\xEB\x9Dm\xD8\x9C|\xD1\xC3\xA1fY0\x09\xAE\x99\xB6\xD4%100644 Makefile\x00V\xC3\xA8\xA01\x0AA1\xEA\x93\xF95q\xA2\x80\x22Ct\x95Wv\x84\xC0\xA1\xA8}\xDBZ\x9FC\xCE\x7F100644 README.adoc\x00\x05\x13\x10\x8F\xFEpT\xF2\xB6e\x1F\xBD-\xB6%\x9C\xAE\x9E\x19\xE2\xDF\x7F\x5C\xEB\x16\xFC\xC8B\x5C\x0Ea\xF040000 doc\x00MS\xF1&a#!\xF2\xB0\xBF\xC6\x0C\x98\xB7\xD5\x04\xF3\x98\xED1\xD8\x22:\x9C[\xAC\xAFmg+L\xC440000 lawn-9p\x00\x91\xE0\x1B\xB7\x5CvSxT\x87@k\xB7\xDBr\x9E\xDAL\x8B{*\x14h\x8F\xE5\xEAU\x03u\xC1\xB2\xFC40000 lawn-constants\x00_\xBE\x0AX\xA2\x89\x08\xBDlmz\xBD\xB6\x13\x08\xDA\x01\xC6\xC7\x94\x01u\x97\x5C\x22\xBE\x0AB\x0B\xDA\x14w40000 lawn-fs\x00\x82\xD9\x87\xA4\xB9ani\xC9]\x88E\x07G#[\x1C\xC1y\x92\xDA\x11\x9F!Cz\xAA\xBB3\x19v\xE440000 lawn-protocol\x00\x95\xF2\xFAp\xBB\x1E4\x08\xC8\xB4X~\xA6Z]E(\xE0\x0E\xC6\xA8%\xAA\xD3\xB3\x13\xCDc\x1C%\x11\xC340000 lawn-sftp\x00{\x95T`\xD7\x87\xD0qW\x82\x3D\xA9\x1A\x86\x83\xA9\x9F\xFC\xB6\xDB\xBDP\xADA\xF2\xCF0\xEFm\x12p\xCA40000 lawn\x00\xAAv\x0A\x05U\xC9\xA1\xC9\x9B\x0DQ\x9B\x1A\xAB\x10\x07rO\xED\x11\x078'\xFA\x88\xBA\x12\xB8U\xC63\x1440000 script\x008X\x159\xF5P\x90(\xF7\x89)<\xC5C\xB3K\xC0w\xB9\x8CsE&\x0A\x00Ys\xA8\x04\x0C\xDB\x0940000 spec\x00\x1DD(\x22\x87;\xAF\xB6\xD9\xFAI\xF8\xF0\x83sW\xF9\x80CN\xCE(v\x1Co\xAC\xD0\xE8Kp\x96\xDB40000 test\x00\xE9\x9DL\xFA-\xCF\xA7\xC6\x99\x1El\x06U\xFE]\xA8#\xC5\xCC\xE7\xC8\xE0G\x0E\x82\x8A\xA5\x9A9C\x05r";
        let expected = br"100644 blob b8013577d726c08b5eb0c01ff59f9e46fbe804c349b9f3910a273daca4b94617	.gitignore
100644 blob 914e78499dfc0b383f89acf1ddaf24f2feba9293e08a883b88247d0ceb4ae882	.rustfmt.toml
100644 blob 98ab49bc506d24eb16e4449c086eaddcd908983f999f1f96c21565b120511760	CHANGELOG.adoc
100644 blob 9e28aa701af9b965386f59a948f55b7d9f881a6d3ed170f423c6613e54aa6833	Cargo.lock
100644 blob a299e69de02d1efb628f9dbf6e8401ca7044e978bc2a393c8c60c41af2693431	Cargo.toml
100644 blob 88253012d1d88eb2e350658b9b0eeb9d6dd89c7cd1c3a166593009ae99b6d425	LICENSE
100644 blob 56c3a8a0310a4131ea93f93571a28022437495577684c0a1a87ddb5a9f43ce7f	Makefile
100644 blob 0513108ffe7054f2b6651fbd2db6259cae9e19e2df7f5ceb16fcc8425c0e61f0	README.adoc
040000 tree 4d53f126612321f2b0bfc60c98b7d504f398ed31d8223a9c5bacaf6d672b4cc4	doc
040000 tree 91e01bb75c7653785487406bb7db729eda4c8b7b2a14688fe5ea550375c1b2fc	lawn-9p
040000 tree 5fbe0a58a28908bd6c6d7abdb61308da01c6c7940175975c22be0a420bda1477	lawn-constants
040000 tree 82d987a4b9616e69c95d88450747235b1cc17992da119f21437aaabb331976e4	lawn-fs
040000 tree 95f2fa70bb1e3408c8b4587ea65a5d4528e00ec6a825aad3b313cd631c2511c3	lawn-protocol
040000 tree 7b955460d787d07157823da91a8683a99ffcb6dbbd50ad41f2cf30ef6d1270ca	lawn-sftp
040000 tree aa760a0555c9a1c99b0d519b1aab1007724fed11073827fa88ba12b855c63314	lawn
040000 tree 38581539f5509028f789293cc543b34bc077b98c7345260a005973a8040cdb09	script
040000 tree 1d442822873bafb6d9fa49f8f0837357f980434ece28761c6facd0e84b7096db	spec
040000 tree e99d4cfa2dcfa7c6991e6c0655fe5da823c5cce7c8e0470e828aa59a39430572	test
";
        let pt = ParseableTree::new(data, HashAlgorithm::SHA256);
        let mut c = Cursor::new(Vec::new());
        for e in pt.iter() {
            write_tree_entry(&mut c, &e.unwrap());
        }
        assert_eq!(c.into_inner(), expected);
    }
}
