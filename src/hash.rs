const GIT_MAX_RAWSZ: usize = 32;

/// A binary object ID.
#[repr(C)]
#[derive(Debug, Clone, Ord, PartialOrd, Eq, PartialEq)]
pub struct ObjectID {
    pub hash: [u8; GIT_MAX_RAWSZ],
    pub algo: u32,
}
