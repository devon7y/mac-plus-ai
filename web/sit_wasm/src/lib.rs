//! A plain C ABI over the `stuffit` crate for the web page (no wasm-bindgen):
//!   p = sit_alloc(n); copy the archive to p; k = sit_unpack(p, n) (number of entries, or < 0);
//!   the result is at sit_out_ptr() .. + sit_out_len(); then sit_free(p, n) and sit_reset().
//! Result layout, per entry (big-endian):
//!   u8 is_folder, u16 name length, name (UTF-8, "/" between folder levels), [4] type, [4] creator,
//!   u16 Finder flags, u32 data length, data, u32 resource length, resource fork.
use stuffit::SitArchive;

static mut OUT: Vec<u8> = Vec::new();

#[no_mangle]
pub extern "C" fn sit_alloc(n: usize) -> *mut u8 {
    let mut v = Vec::<u8>::with_capacity(n.max(1));
    let p = v.as_mut_ptr();
    std::mem::forget(v);
    p
}

#[no_mangle]
pub unsafe extern "C" fn sit_free(p: *mut u8, n: usize) {
    drop(Vec::from_raw_parts(p, 0, n.max(1)));
}

#[no_mangle]
pub extern "C" fn sit_out_ptr() -> *const u8 {
    unsafe { (*std::ptr::addr_of!(OUT)).as_ptr() }
}

#[no_mangle]
pub extern "C" fn sit_out_len() -> usize {
    unsafe { (*std::ptr::addr_of!(OUT)).len() }
}

#[no_mangle]
pub extern "C" fn sit_reset() {
    unsafe { *std::ptr::addr_of_mut!(OUT) = Vec::new(); }
}

/// -1: not a StuffIt archive (or damaged); -2: an entry could not be decompressed.
#[no_mangle]
pub unsafe extern "C" fn sit_unpack(p: *const u8, n: usize) -> i32 {
    let data = std::slice::from_raw_parts(p, n);
    let arc = match SitArchive::parse_auto(data) { Ok(a) => a, Err(_) => return -1 };
    let mut out = Vec::new();
    for e in &arc.entries {
        let (d, r) = if e.is_folder { (Vec::new(), Vec::new()) } else {
            match e.decompressed_forks() { Ok(x) => x, Err(_) => return -2 }
        };
        out.push(e.is_folder as u8);
        let nm = e.name.as_bytes();
        out.extend_from_slice(&(nm.len() as u16).to_be_bytes());
        out.extend_from_slice(nm);
        out.extend_from_slice(&e.file_type);
        out.extend_from_slice(&e.creator);
        out.extend_from_slice(&e.finder_flags.to_be_bytes());
        out.extend_from_slice(&(d.len() as u32).to_be_bytes());
        out.extend_from_slice(&d);
        out.extend_from_slice(&(r.len() as u32).to_be_bytes());
        out.extend_from_slice(&r);
    }
    *std::ptr::addr_of_mut!(OUT) = out;
    arc.entries.len() as i32
}
