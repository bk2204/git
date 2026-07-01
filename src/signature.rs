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

use crate::hash::HashAlgorithm;
use crate::object::header::{HeaderObjectParser, ParsedObjectKind};
use std::io::Write;

/// Valid signature formats.
#[repr(C)]
#[derive(Debug, Copy, Clone, Ord, PartialOrd, Eq, PartialEq)]
pub enum SignatureFormat {
    /// The OpenPGP signature format, using `gpg` as the interface.
    OpenPGP = 0,
    /// The X.509 signature format, using `gpgsm` as the interface.
    X509 = 1,
    /// The SSH signature format, using `ssh-keygen` as the interface.
    SSH = 2,
}

impl SignatureFormat {
    /// A slice of all signature formats.
    pub fn all_formats() -> &'static [SignatureFormat] {
        &[
            SignatureFormat::OpenPGP,
            SignatureFormat::X509,
            SignatureFormat::SSH,
        ]
    }

    /// The header lines which indicate a signature in each of the formats.
    pub fn headers(self) -> &'static [&'static str] {
        match self {
            Self::OpenPGP => &[
                "-----BEGIN PGP SIGNATURE-----",
                "-----BEGIN PGP MESSAGE-----",
            ],
            Self::X509 => &["-----BEGIN SIGNED MESSAGE-----"],
            Self::SSH => &["-----BEGIN SSH SIGNATURE-----"],
        }
    }

    /// Find the signature format that this line belongs to.
    ///
    /// If no signature format corresponds to this line, returns `None`.
    pub fn format_by_header(line: &[u8]) -> Option<SignatureFormat> {
        // This is an optimization for large tag bodies.
        if !line.starts_with(b"-----BEGIN ") {
            return None;
        }
        for fmt in Self::all_formats() {
            for header in fmt.headers() {
                if line.starts_with(header.as_bytes()) {
                    return Some(*fmt);
                }
            }
        }
        None
    }
}

/// Parse a buffer with an optional signature on the end, as for tags.
///
/// Returns a tuple, where the first item is the non-signature data and the second item is the
/// optional signature, or `None` if ther eis no signature.
pub fn parse_signed_buffer(data: &[u8]) -> (&[u8], Option<&[u8]>) {
    let mut last_off = 0;
    let mut found = (data, None);
    for (off, _) in data.iter().enumerate().filter(|(_, &b)| b == b'\n') {
        let line = &data[last_off..off];
        if SignatureFormat::format_by_header(line).is_some() {
            found = (&data[..last_off], Some(&data[last_off..]));
        }
        last_off = off + 1;
        if last_off >= data.len() {
            break;
        }
    }
    found
}

pub fn parse_buffer_signed_by_header<W: Write>(
    data: &[u8],
    algo: HashAlgorithm,
    payload: &mut W,
    signature: &mut W,
) -> bool {
    let mut saw_signature = false;
    let parser = HeaderObjectParser::new(data, true, true);
    for obj in parser {
        let obj = match obj {
            Ok(obj) => obj,
            // We ignore errors in the original C as well.
            Err(_) => return false,
        };
        let kind = obj.kind;
        match kind {
            ParsedObjectKind::Header(header, body) if header == algo.gpgsig_header().as_bytes() => {
                let _ = signature.write(&body);
                let _ = signature.write(b"\n");
                saw_signature = true;
            }
            ParsedObjectKind::Header(header, _) if header.starts_with(b"gpgsig") => {
                // Ignore other signatures.
            }
            ParsedObjectKind::Header(_, _) => {
                let _ = payload.write(&data[obj.start..obj.end]);
            }
            ParsedObjectKind::Body => {
                let _ = payload.write(&data[obj.start..obj.end]);
                return saw_signature;
            }
        }
    }
    saw_signature
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::strbuf::Strbuf;
    use crate::util::tests::strip_q;

    #[test]
    fn parse_buffer_signed_by_header_cases() {
        let testcases: &[(&str, &[u8], &[u8], &[u8], HashAlgorithm, bool)] = &[
            (
                "no signature",
                b"tree 1111111111111111111111111111111111111111
parent 2222222222222222222222222222222222222222
parent 3333333333333333333333333333333333333333
author A U Thor <author@example.com> 1758234120 -0700
committer C O Mitter <committer@example.com> 1758234120 -0700

Commit message.

More lines.
",
                b"tree 1111111111111111111111111111111111111111
parent 2222222222222222222222222222222222222222
parent 3333333333333333333333333333333333333333
author A U Thor <author@example.com> 1758234120 -0700
committer C O Mitter <committer@example.com> 1758234120 -0700

Commit message.

More lines.
",
                b"",
                HashAlgorithm::SHA1,
                false,
            ),
            (
                "single main signature",
                b"tree 1111111111111111111111111111111111111111111111111111111111111111
parent 2222222222222222222222222222222222222222222222222222222222222222
parent 3333333333333333333333333333333333333333333333333333333333333333
author A U Thor <author@example.com> 1758234120 -0700
committer C O Mitter <committer@example.com> 1758234120 -0700
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

Commit message.

More lines.
",
                b"-----BEGIN PGP SIGNATURE-----

iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
=BBBB
-----END PGP SIGNATURE-----
",
                HashAlgorithm::SHA256,
                true,
            ),
            (
                "complex multi-signed commit with mergetag",
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

Commit message.

More lines.
",
                b"-----BEGIN PGP SIGNATURE-----

iIoEABYKADIWIQRRSw7ePDh/lE+zeZMp5XQQmuv6qgUCaMxsBRQccm9zdGVkdEBn
b29kbWlzLm9yZwAKCRAp5XQQmuv6qk5oAP4tlGnMBoLpZXVBpAubVUQOVRfQo5dI
ar9LpXdgnj4xQAEA9Q5uIvhCI/CMXTK98gFhR31p9O4Sqtn0JlCViBbVSQg=
=BBBB
-----END PGP SIGNATURE-----
",
                HashAlgorithm::SHA256,
                true,
            ),
        ];

        for (desc, input, payload, signature, algo, saw_sig) in testcases {
            let mut p = Strbuf::new();
            let mut sig = Strbuf::new();
            let input = strip_q(&input);
            assert_eq!(
                parse_buffer_signed_by_header(&input, *algo, &mut p, &mut sig),
                *saw_sig,
                "{}: return is correct",
                desc
            );
            let payload = strip_q(payload);
            let signature = strip_q(signature);
            assert_eq!(payload, p.to_bytes(), "{}: payload is correct", desc);
            assert_eq!(signature, sig.to_bytes(), "{}: signature is correct", desc);
        }
    }

    #[test]
    fn parses_signed_buffer() {
        let signed: &[u8] = b"object e156455ea49124c140a67623f22a393db62d5d98
type commit
tag v2.0.0
tagger Junio C Hamano <gitster@pobox.com> 1401300269 -0700

Git 2.0
-----BEGIN PGP SIGNATURE-----
Version: GnuPG v1.4.11 (GNU/Linux)

iQIcBAABAgAGBQJThiUtAAoJELC16IaWr+bLqmwP/1/xdI8V9FicGmi5D0mfMd3C
Ixd4JkpKmFrErax0XJO0S7xiJ/niK/4tmvqJ/6D4v7ZF0Ks+g+33IAaZqP38MLpa
rPOi8yZ/9qFWgjBWT52aN35Rx+JKlIjwJyeuxumKgYa3bb/TrrAOiy+PeMO6tCfH
fcl5gtIE/KFDZ0xQ+FuwWJ72erwT7V3lHwocVoET1T16A8MVnbuL7nVaH+dcoAmx
FPMTl/s0KIBRRgEq4twE94T2J4O8i3Sb0fUqbeSjtOQ8pMYMz351PjVWf2mTs8rO
SZrF+ohJX46y7HRf+VA2sNeoHx1Zvq3hn/g4wkh5swAXbLcrE6EqS0E4CN5bujQu
YDsCeaCb2ZSvfFAvE4s5q69jgG5/qp5GkIni5/leFuEEETXZkGb8CVeWeKrB7JLQ
7Hem4xoljEF2cAAUjiIP1SvjT0X9bvKbfioG3yTK7C037YzZcdsnghEiUxV4ypkT
93VuPi7F2XHkB+NnHAwcSuCjqvNZWK0azwwqprFHROlTCJ4T4mRpRQXcpTUSqSch
Oj8d9NyTK3GTZRCHmHVWOyTISVji1uHaovx4jwVJmNvLqS3aZAEKoMUyPRh2EAfd
vZxlxVpBOiCefzgfV8+M1O4kjdakKT1yqVB6dFbWJGrZGZM9JVoKj+7JhR25QSzl
wAESIYo0I+10Ida57ciU
=QySQ
-----END PGP SIGNATURE-----
";
        let unsigned: &[u8] = b"object e156455ea49124c140a67623f22a393db62d5d98
type commit
tag v2.0.0
tagger Junio C Hamano <gitster@pobox.com> 1401300269 -0700

Git 2.0
";
        let sig: &[u8] = b"-----BEGIN PGP SIGNATURE-----
Version: GnuPG v1.4.11 (GNU/Linux)

iQIcBAABAgAGBQJThiUtAAoJELC16IaWr+bLqmwP/1/xdI8V9FicGmi5D0mfMd3C
Ixd4JkpKmFrErax0XJO0S7xiJ/niK/4tmvqJ/6D4v7ZF0Ks+g+33IAaZqP38MLpa
rPOi8yZ/9qFWgjBWT52aN35Rx+JKlIjwJyeuxumKgYa3bb/TrrAOiy+PeMO6tCfH
fcl5gtIE/KFDZ0xQ+FuwWJ72erwT7V3lHwocVoET1T16A8MVnbuL7nVaH+dcoAmx
FPMTl/s0KIBRRgEq4twE94T2J4O8i3Sb0fUqbeSjtOQ8pMYMz351PjVWf2mTs8rO
SZrF+ohJX46y7HRf+VA2sNeoHx1Zvq3hn/g4wkh5swAXbLcrE6EqS0E4CN5bujQu
YDsCeaCb2ZSvfFAvE4s5q69jgG5/qp5GkIni5/leFuEEETXZkGb8CVeWeKrB7JLQ
7Hem4xoljEF2cAAUjiIP1SvjT0X9bvKbfioG3yTK7C037YzZcdsnghEiUxV4ypkT
93VuPi7F2XHkB+NnHAwcSuCjqvNZWK0azwwqprFHROlTCJ4T4mRpRQXcpTUSqSch
Oj8d9NyTK3GTZRCHmHVWOyTISVji1uHaovx4jwVJmNvLqS3aZAEKoMUyPRh2EAfd
vZxlxVpBOiCefzgfV8+M1O4kjdakKT1yqVB6dFbWJGrZGZM9JVoKj+7JhR25QSzl
wAESIYo0I+10Ida57ciU
=QySQ
-----END PGP SIGNATURE-----
";
        assert_eq!(parse_signed_buffer(signed), (unsigned, Some(sig)));
        assert_eq!(parse_signed_buffer(unsigned), (unsigned, None));
    }
}
