#[repr(C)]
#[derive(Copy, Clone)]
pub struct Uint128 {
    pub little_endian_bytes: [u8; 16],
}

impl core::fmt::Debug for Uint128 {
    #[inline]
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        u128::from_le_bytes(self.little_endian_bytes).fmt(f)
    }
}

pub struct Csprng {
    __private: (),
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct CsprngVtable {
    /// Returns the number of remaining bytes that this Csprng can generate.
    pub remaining_bytes: unsafe extern "C" fn(csprng: *const Csprng) -> Uint128,

    /// Fills the byte array with random bytes, up to the given count, and returns the count of
    /// successfully generated bytes.
    pub next_bytes:
        unsafe extern "C" fn(csprng: *mut Csprng, byte_array: *mut u8, byte_count: usize) -> usize,
}

#[repr(u32)]
#[derive(Copy, Clone, Debug)]
pub enum ScratchStatus {
    Valid = 0,
    SizeOverflow = 1,
}

#[repr(u32)]
#[derive(Copy, Clone, Debug)]
pub enum Parallelism {
    No = 0,
    Rayon = 1,
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::c_api::csprng::CONCRETE_CSPRNG_VTABLE;
    use crate::implementation::types::CsprngMut;
    use concrete_csprng::generators::SoftwareRandomGenerator;

    pub fn to_generic(a: &mut SoftwareRandomGenerator) -> CsprngMut<'_, '_> {
        unsafe {
            CsprngMut::new(
                a as *mut SoftwareRandomGenerator as *mut Csprng,
                &CONCRETE_CSPRNG_VTABLE,
            )
        }
    }
}
