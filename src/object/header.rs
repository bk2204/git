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

use std::borrow::Cow;

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct InvalidHeader;

/// The kind of object parsed from an object with header.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum ParsedObjectKind<'a> {
    /// An object header.
    ///
    /// The first component of the tuple is the header name and the second is the body of the
    /// header.
    Header(&'a [u8], Cow<'a, [u8]>),
    /// The body of the object.
    Body,
}

/// A piece of a parsed object.
///
/// `kind` is the kind of component parsed, either a header or a body.  `start` is the starting
/// offset of the object in the original slice and `end` is the end of the object, including the
/// trailing newline, if any.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct ParsedObjectComponent<'a> {
    pub kind: ParsedObjectKind<'a>,
    pub start: usize,
    pub end: usize,
}

impl<'a> ParsedObjectComponent<'a> {
    fn new(kind: ParsedObjectKind<'a>, start: usize, end: usize) -> Self {
        Self { kind, start, end }
    }
}

/// A parser for commits and tags.
pub struct HeaderObjectParser<'a> {
    data: &'a [u8],
    multiline: bool,
    strip_continuation: bool,
    offset: usize,
}

impl<'a> HeaderObjectParser<'a> {
    /// Create a new parse.
    ///
    /// If `multiline` is true, allow a multiline header with space continuation.  If
    /// `strip_continuation` is also true, strip the leading space from continuation lines.
    pub fn new(data: &'a [u8], multiline: bool, strip_continuation: bool) -> Self {
        Self {
            data,
            multiline,
            strip_continuation,
            offset: 0,
        }
    }
}

impl<'a> Iterator for HeaderObjectParser<'a> {
    type Item = Result<ParsedObjectComponent<'a>, InvalidHeader>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.offset >= self.data.len() {
            return None;
        }
        let data = &self.data[self.offset..];
        // TODO: use split_inclusive when we allow Rust 1.51.  That simplifies a large amount of
        // code below which must manually account for the presence or absence of the newline in the
        // input data.
        let mut lines = data.split(|b| *b == b'\n').peekable();
        let first = match lines.next() {
            Some(line) => line,
            // The object ends immediately after the header with no message.
            None => {
                let offset = self.offset;
                self.offset = self.data.len();
                return Some(Ok(ParsedObjectComponent::new(
                    ParsedObjectKind::Body,
                    offset,
                    self.data.len(),
                )))
            }
        };
        // We've reached the end of the header since this is an empty line.
        if first.is_empty() {
            let offset = self.offset;
            self.offset = self.data.len();
            return Some(Ok(ParsedObjectComponent::new(
                ParsedObjectKind::Body,
                offset,
                self.data.len(),
            )));
        }
        let mut header_pieces = first.splitn(2, |b| *b == b' ');
        let header_name = match header_pieces.next() {
            Some(name) => name,
            None => return Some(Err(InvalidHeader)),
        };
        // If there is no last item, then the header name is empty, which is not allowed.
        let header_last = match header_name.last() {
            Some(last) => last,
            None => return Some(Err(InvalidHeader)),
        };
        let space_offset = crate::util::element_offset(data, header_last).unwrap() + 1;
        let header = &data[0..space_offset];
        if first.len() <= space_offset {
            return Some(Err(InvalidHeader));
        }
        let body = &first[space_offset + 1..first.len()];
        let mut offset = first.len();
        if data.len() > first.len() {
            offset += 1;
        }
        if !lines.peek().map_or(false, |&l| l.starts_with(b" ")) || !self.multiline {
            let (start, end) = (self.offset, self.offset + offset);
            self.offset = end;
            return Some(Ok(ParsedObjectComponent::new(
                ParsedObjectKind::Header(header, Cow::Borrowed(body)),
                start,
                end,
            )));
        }
        let mut offset = first.len();
        let mut trim_offset = if data.len() > first.len() {
            offset += 1;
            1
        } else {
            0
        };
        if self.strip_continuation {
            let mut body = first[space_offset + 1..].to_owned();
            for line in lines {
                if line.is_empty() {
                    // We've reached the end of the header since this is an empty line.
                    break;
                }
                if line.starts_with(b" ") {
                    offset += line.len();
                    if data.len() > offset {
                        offset += 1;
                    }
                    body.push(b'\n');
                    body.extend(&line[1..]);
                } else {
                    break;
                }
            }
            let (start, end) = (self.offset, self.offset + offset);
            self.offset = end;
            Some(Ok(ParsedObjectComponent::new(
                ParsedObjectKind::Header(header, Cow::Owned(body)),
                start,
                end,
            )))
        } else {
            for line in lines {
                if line.is_empty() {
                    // We've reached the end of the header since this is an empty line.
                    break;
                }
                if line.starts_with(b" ") {
                    offset += line.len();
                    if data.len() > offset {
                        offset += 1;
                        trim_offset = 1;
                    } else {
                        trim_offset = 0;
                    }
                } else {
                    break;
                }
            }
            let (start, end) = (self.offset, self.offset + offset);
            self.offset = end;
            Some(Ok(ParsedObjectComponent::new(
                ParsedObjectKind::Header(
                    header,
                    Cow::Borrowed(&data[space_offset + 1..offset - trim_offset]),
                ),
                start,
                end,
            )))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_header_simple() {
        let testcases: &[(&[u8], <HeaderObjectParser as Iterator>::Item)] = &[
            (b"tree", Err(InvalidHeader)),
            (b"tree\n", Err(InvalidHeader)),
            (
                b"tree ",
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(b"tree", Cow::Borrowed(b"")),
                    start: 0,
                    end: 5,
                }),
            ),
            (
                b"tree \n",
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(b"tree", Cow::Borrowed(b"")),
                    start: 0,
                    end: 6,
                }),
            ),
            (
                b"tree 13436321328fa7a8cb4f80b1d8110542e926fcb5ba946ec22240aed735d90708\n",
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(
                        b"tree",
                        Cow::Borrowed(
                            b"13436321328fa7a8cb4f80b1d8110542e926fcb5ba946ec22240aed735d90708",
                        ),
                    ),
                    start: 0,
                    end: 70,
                }),
            ),
            (
                b"tree 13436321328fa7a8cb4f80b1d8110542e926fcb5ba946ec22240aed735d90708",
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(
                        b"tree",
                        Cow::Borrowed(
                            b"13436321328fa7a8cb4f80b1d8110542e926fcb5ba946ec22240aed735d90708",
                        ),
                    ),
                    start: 0,
                    end: 69,
                }),
            ),
        ];

        for (data, result) in testcases {
            let mut parser = HeaderObjectParser::new(data, false, false);
            assert_eq!(parser.next().unwrap(), *result);
            let mut parser = HeaderObjectParser::new(data, true, false);
            assert_eq!(parser.next().unwrap(), *result);
            let mut parser = HeaderObjectParser::new(data, true, true);
            assert_eq!(parser.next().unwrap(), *result);
        }
    }

    #[test]
    fn parse_header_multiline() {
        let testcases: &[(&[u8], bool, <HeaderObjectParser as Iterator>::Item)] = &[
            (
                b"gpgsig -----BEGIN PGP SIGNATURE-----\n \n wr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\n LnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\n FiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\n ygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n =xWdn\n -----END PGP SIGNATURE-----",
                false,
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(
                    b"gpgsig",
                    Cow::Borrowed(
                        b"-----BEGIN PGP SIGNATURE-----\n \n wr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\n LnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\n FiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\n ygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n =xWdn\n -----END PGP SIGNATURE-----",
                    )),
                    start: 0,
                    end: 338,
                }),
            ),
            (
                b"gpgsig -----BEGIN PGP SIGNATURE-----\n \n wr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\n LnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\n FiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\n ygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n =xWdn\n -----END PGP SIGNATURE-----\n",
                false,
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(
                    b"gpgsig",
                    Cow::Borrowed(
                        b"-----BEGIN PGP SIGNATURE-----\n \n wr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\n LnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\n FiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\n ygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n =xWdn\n -----END PGP SIGNATURE-----",
                    )),
                    start: 0,
                    end: 339,
                }),
            ),
            (
                b"gpgsig -----BEGIN PGP SIGNATURE-----\n \n wr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\n LnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\n FiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\n ygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n =xWdn\n -----END PGP SIGNATURE-----",
                true,
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(
                    b"gpgsig",
                    Cow::Borrowed(
                        b"-----BEGIN PGP SIGNATURE-----\n\nwr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\nLnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\nFiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\nygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n=xWdn\n-----END PGP SIGNATURE-----",
                    )),
                    start: 0,
                    end: 338,
                }),
            ),
            (
                b"gpgsig -----BEGIN PGP SIGNATURE-----\n \n wr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\n LnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\n FiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\n ygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n =xWdn\n -----END PGP SIGNATURE-----\n",
                true,
                Ok(ParsedObjectComponent {
                    kind: ParsedObjectKind::Header(
                    b"gpgsig",
                    Cow::Borrowed(
                        b"-----BEGIN PGP SIGNATURE-----\n\nwr0EABYKAG8Fgmo0Zf4JEHwMSWKIh6KBRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z\nLnNlcXVvaWEtcGdwLm9yZxJjn8tz1MABD8xhkpd/aMvPj9lZkUmuy9M0oH0PKtep\nFiEECCzmip28ZfuD0cORfAxJYoiHooEAAFLQAP0WVKxPTN6AByJa6XlE9UyBnVs6\nygijAq4QMHymw6hOCwD+J3v9ycUMEL+X9PV+bZjw1/5KQtoAErVFzJPChg9pywc=\n=xWdn\n-----END PGP SIGNATURE-----",
                    )),
                    start: 0,
                    end: 339,
                }),
            ),
        ];

        for (data, strip_continuation, result) in testcases {
            let mut parser = HeaderObjectParser::new(data, true, *strip_continuation);
            assert_eq!(parser.next().unwrap(), *result);
        }
    }
}
