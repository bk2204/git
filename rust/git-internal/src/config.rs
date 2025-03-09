#![allow(dead_code)]

use crate::convert::{AsDisplayStr, OsFromBytes};
use crate::error::Error;
use std::any::Any;
use std::collections::BTreeMap;
use std::ffi::{CStr, CString, OsStr};
use std::fmt;
use std::io::{BufReader, Bytes, Read};
use std::os::raw::{c_char, c_int};
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd)]
pub enum ConfigScope {
    Unknown = 0,
    System,
    Global,
    Local,
    Worktree,
    Command,
    Submodule,
}

impl fmt::Display for ConfigScope {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
        let s = match self {
            ConfigScope::System => "system",
            ConfigScope::Global => "global",
            ConfigScope::Local => "local",
            ConfigScope::Worktree => "worktree",
            ConfigScope::Command => "command",
            ConfigScope::Submodule => "submodule",
            _ => "unknown",
        };
        write!(f, "{}", s)
    }
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd)]
pub enum ConfigOrigin {
    Unknown = 0,
    Blob,
    File,
    Stdin,
    SubmoduleBlob,
    Cmdline,
}

impl fmt::Display for ConfigOrigin {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
        let s = match self {
            ConfigOrigin::Blob => "blob",
            ConfigOrigin::File => "file",
            ConfigOrigin::Stdin => "standard input",
            ConfigOrigin::SubmoduleBlob => "submodule-blob",
            ConfigOrigin::Cmdline => "command line",
            _ => "unknown",
        };
        write!(f, "{}", s)
    }
}

#[repr(C)]
#[derive(Copy, Clone, Eq, PartialEq, Ord, PartialOrd)]
pub enum ConfigEvent {
    Section,
    Entry,
    Whitespace,
    Comment,
    Eof,
    Error,
}

/// Config source metadata for a given config key-value pair.
#[repr(C)]
pub struct KeyValueInfo {
    filename: *const c_char,
    linenr: c_int,
    origin_type: ConfigOrigin,
    scope: ConfigScope,
    path: *const c_char,
}

/// Captures additional information that a config callback can use.
#[repr(C)]
pub struct ConfigContext {
    /// Config source metadata for key and value.
    kvi: *const KeyValueInfo,
}

pub struct ConfigSource {
    rdr: Option<Bytes<BufReader<Box<dyn Read>>>>,
    name: Option<String>,
    path: Option<PathBuf>,
    cpath: Option<CString>,
    lineno: u64,
    total_len: u64,
    value: Vec<u8>,
    var: Vec<u8>,
    subsection_case_sensitive: bool,
    origin_type: ConfigOrigin,
    scope: ConfigScope,
}

impl ConfigSource {
    pub fn new(
        rdr: Box<dyn Read>,
        name: Option<&[u8]>,
        path: Option<&[u8]>,
        origin_type: ConfigOrigin,
        scope: ConfigScope,
    ) -> Result<Self, Error> {
        let bufrdr = BufReader::new(rdr);
        Ok(ConfigSource {
            rdr: Some(bufrdr.bytes()),
            name: name.map(|n| n.as_display_str().to_string()),
            path: match path {
                Some(p) => Some(Path::new(&OsStr::from_bytes(p)?).to_path_buf()),
                None => None,
            },
            cpath: path.map(|p| CString::new(p).unwrap()),
            lineno: 0,
            total_len: 0,
            value: Vec::new(),
            var: Vec::new(),
            subsection_case_sensitive: false,
            origin_type,
            scope,
        })
    }
}

/// The type of a parsed value.
#[derive(Debug)]
pub enum ParserValue {
    String(CString),
    Bool(bool),
    Int(i64),
    Any(Box<dyn Any>),
}

impl PartialEq for ParserValue {
    fn eq(&self, other: &ParserValue) -> bool {
        match (self, other) {
            (Self::String(a), Self::String(b)) => a == b,
            (Self::Bool(a), Self::Bool(b)) => a == b,
            (Self::Int(a), Self::Int(b)) => a == b,
            _ => false,
        }
    }
}

/// A response parsed from a callback.
///
/// This represents the value parsed by a callback.  If the variant is `Value`, then the value
/// replaces any existing value that may be set.  If the value is `Append`, then the item is
/// appended to the list of the values.  If the value is `Reset`, then the list of values is reset.
pub enum ParserValueResponse {
    Value(ParserValue),
    Append(ParserValue),
    Reset,
}

pub type ConfigParserCallback =
    dyn FnMut(&CStr, Option<&CStr>, &ConfigContext) -> Result<Option<ParserValueResponse>, Error>;

pub type ConfigEntries = BTreeMap<CString, Vec<ParserValue>>;

pub struct ConfigParser {
    source: ConfigSource,
    callback: Box<ConfigParserCallback>,
    parsed_entries: Arc<RwLock<ConfigEntries>>,
}

impl ConfigParser {
    pub fn new(source: ConfigSource, callback: Box<ConfigParserCallback>) -> ConfigParser {
        ConfigParser {
            source,
            callback,
            parsed_entries: Default::default(),
        }
    }

    pub fn parse(&mut self) -> Result<(), Error> {
        unimplemented!()
    }

    pub fn into_config_entries(self) -> Arc<RwLock<ConfigEntries>> {
        self.parsed_entries
    }
}
