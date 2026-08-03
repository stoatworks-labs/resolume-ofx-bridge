//! A minimal After Effects effect HOST — the AE-as-guest cells of the
//! any-to-any matrix.
//!
//! This stands where After Effects normally stands: it loads a .plugin's
//! `EffectMain`, walks it through GLOBAL_SETUP / PARAMS_SETUP / RENDER, and
//! supplies the slice of the host environment a well-behaved effect actually
//! touches — the `add_param` interact callback, the `iterate` util callback,
//! the pica Handle Suite, and round-tripped global/sequence data.
//!
//! It is deliberately minimal, in the same spirit as the bridge's OFX host
//! when it started: effects that demand deeper application services (custom
//! UI, AEGP suites, licence checks against a running AE) are reported as
//! unsupported by name, not crashed into. Every unknown suite request is
//! logged to stderr so the next capability to add is always evidence, not
//! guesswork.
//!
//! Struct layouts come from the community `after-effects-sys` bindings; no
//! Adobe SDK is involved. Frames cross the C ABI as RGBA8; PF worlds are
//! ARGB order internally, converted here at the boundary.

use after_effects_sys as ae;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::Mutex;

// ---------------------------------------------------------------------------
// Handle suite: PF_Handle is a pointer to a pointer to the data. Allocations
// are leaked into a registry rather than freed on dispose-at-exit paths — the
// fleet's exit-teardown scars apply here too, and a host outlives its guests.
// ---------------------------------------------------------------------------

static HANDLES: Mutex<Vec<usize>> = Mutex::new(Vec::new());

unsafe extern "C" fn host_new_handle(size: ae::A_HandleSize) -> ae::PF_Handle {
    let boxed: Box<Vec<u8>> = Box::new(vec![0u8; size as usize]);
    let data_holder = Box::into_raw(boxed);
    HANDLES.lock().unwrap().push(data_holder as usize);
    // PF_Handle = void**: dereferencing once yields the data pointer.
    let indirection: Box<*mut c_void> = Box::new((*data_holder).as_mut_ptr() as *mut c_void);
    Box::into_raw(indirection) as ae::PF_Handle
}

unsafe extern "C" fn host_lock_handle(h: ae::PF_Handle) -> *mut c_void {
    if h.is_null() {
        return std::ptr::null_mut();
    }
    *(h as *mut *mut c_void)
}

unsafe extern "C" fn host_unlock_handle(_h: ae::PF_Handle) {}

unsafe extern "C" fn host_dispose_handle(_h: ae::PF_Handle) {
    // Leaked on purpose; see the module comment.
}

unsafe extern "C" fn host_get_handle_size(h: ae::PF_Handle) -> ae::A_HandleSize {
    if h.is_null() {
        return 0;
    }
    // Sizes are not tracked per handle; effects that resize query first. The
    // registry keeps Vec sizes, so find it.
    let data = *(h as *mut *mut c_void);
    for holder in HANDLES.lock().unwrap().iter() {
        let holder = *holder as *mut Vec<u8>;
        if (*holder).as_ptr() as *mut c_void == data {
            return (*holder).len() as ae::A_HandleSize;
        }
    }
    0
}

unsafe extern "C" fn host_resize_handle(
    new_size: ae::A_HandleSize,
    handle: *mut ae::PF_Handle,
) -> ae::PF_Err {
    if handle.is_null() {
        return ae::PF_Err_BAD_CALLBACK_PARAM as ae::PF_Err;
    }
    let fresh = host_new_handle(new_size);
    // Contents deliberately not copied: the one caller pattern in the wild
    // resizes then rewrites. If an effect turns out to depend on preserved
    // contents, this is the line the evidence will point at.
    *handle = fresh;
    ae::PF_Err_NONE as ae::PF_Err
}

// ---------------------------------------------------------------------------
// SPBasic: the suite broker. Serve the Handle Suite; name anything else.
// ---------------------------------------------------------------------------

unsafe extern "C" fn sp_acquire(
    name: *const c_char,
    version: i32,
    suite: *mut *const c_void,
) -> ae::SPErr {
    let requested = if name.is_null() {
        String::new()
    } else {
        CStr::from_ptr(name).to_string_lossy().into_owned()
    };

    // Adobe's version macros are not what they look like: kPFHandleSuiteVersion1
    // is defined as 2 in current SDK generations. Serve the one struct for both.
    if requested.as_bytes() == &ae::kPFHandleSuite[..ae::kPFHandleSuite.len() - 1] && (1..=2).contains(&version) {
        static HANDLE_SUITE: ae::PF_HandleSuite1 = ae::PF_HandleSuite1 {
            host_new_handle: Some(host_new_handle),
            host_lock_handle: Some(host_lock_handle),
            host_unlock_handle: Some(host_unlock_handle),
            host_dispose_handle: Some(host_dispose_handle),
            host_get_handle_size: Some(host_get_handle_size),
            host_resize_handle: Some(host_resize_handle),
        };
        *suite = &HANDLE_SUITE as *const _ as *const c_void;
        return 0;
    }

    eprintln!("aeguest: guest asked for unimplemented suite \"{requested}\" v{version}");
    *suite = std::ptr::null();
    -1 // kSPSuiteNotFoundError-ish: any nonzero reads as "not available"
}

unsafe extern "C" fn sp_release(_name: *const c_char, _version: i32) -> ae::SPErr {
    0
}

unsafe extern "C" fn sp_is_equal(a: *const c_char, b: *const c_char) -> ae::SPBoolean {
    (!a.is_null() && !b.is_null() && CStr::from_ptr(a) == CStr::from_ptr(b)) as ae::SPBoolean
}

unsafe extern "C" fn sp_allocate(size: usize, block: *mut *mut c_void) -> ae::SPErr {
    *block = libc_malloc(size);
    0
}
unsafe extern "C" fn sp_free(block: *mut c_void) -> ae::SPErr {
    libc_free(block);
    0
}
unsafe extern "C" fn sp_realloc(
    block: *mut c_void,
    new_size: usize,
    new_block: *mut *mut c_void,
) -> ae::SPErr {
    *new_block = libc_realloc(block, new_size);
    0
}

extern "C" {
    #[link_name = "malloc"]
    fn libc_malloc(size: usize) -> *mut c_void;
    #[link_name = "free"]
    fn libc_free(p: *mut c_void);
    #[link_name = "realloc"]
    fn libc_realloc(p: *mut c_void, size: usize) -> *mut c_void;
}

static SP_BASIC: ae::SPBasicSuite = ae::SPBasicSuite {
    AcquireSuite: Some(sp_acquire),
    ReleaseSuite: Some(sp_release),
    IsEqual: Some(sp_is_equal),
    AllocateBlock: Some(sp_allocate),
    FreeBlock: Some(sp_free),
    ReallocateBlock: Some(sp_realloc),
    Undefined: None,
};

// ---------------------------------------------------------------------------
// Interact callbacks: PARAMS_SETUP hands us the parameter table.
// ---------------------------------------------------------------------------

unsafe extern "C" fn cb_add_param(
    effect_ref: ae::PF_ProgPtr,
    _index: ae::PF_ParamIndex,
    def: ae::PF_ParamDefPtr,
) -> ae::PF_Err {
    // effect_ref is our Guest, threaded through in_data.effect_ref.
    let guest = &mut *(effect_ref as *mut Guest);
    guest.params.push(*def);
    ae::PF_Err_NONE as ae::PF_Err
}

// ---------------------------------------------------------------------------
// Util callbacks: iterate is the one a CPU effect lives on.
// ---------------------------------------------------------------------------

unsafe extern "C" fn cb_iterate8(
    _in_data: *mut ae::PF_InData,
    _progress_base: ae::A_long,
    _progress_final: ae::A_long,
    src: *mut ae::PF_EffectWorld,
    area: *const ae::PF_Rect,
    refcon: *mut c_void,
    pix_fn: ae::PF_IteratePixel8Func,
    dst: *mut ae::PF_EffectWorld,
) -> ae::PF_Err {
    let Some(pix_fn) = pix_fn else {
        return ae::PF_Err_BAD_CALLBACK_PARAM as ae::PF_Err;
    };
    let dst = &mut *dst;

    let (x0, y0, x1, y1) = if area.is_null() {
        (0, 0, dst.width, dst.height)
    } else {
        ((*area).left as i32, (*area).top as i32, (*area).right as i32, (*area).bottom as i32)
    };

    for y in y0..y1 {
        let dst_row = (dst.data as *mut u8).offset((y * dst.rowbytes) as isize) as *mut ae::PF_Pixel;
        let src_row = if src.is_null() {
            std::ptr::null_mut()
        } else {
            ((*src).data as *mut u8).offset((y * (*src).rowbytes) as isize) as *mut ae::PF_Pixel
        };
        for x in x0..x1 {
            let src_px = if src_row.is_null() { std::ptr::null_mut() } else { src_row.add(x as usize) };
            let err = pix_fn(refcon, x, y, src_px, dst_row.add(x as usize));
            if err != ae::PF_Err_NONE as ae::PF_Err {
                return err;
            }
        }
    }
    ae::PF_Err_NONE as ae::PF_Err
}

// ---------------------------------------------------------------------------
// The guest itself.
// ---------------------------------------------------------------------------

type EffectMainFn = unsafe extern "C" fn(
    ae::PF_Cmd,
    *mut ae::PF_InData,
    *mut ae::PF_OutData,
    *mut ae::PF_ParamDef,
    *mut ae::PF_LayerDef,
    *mut c_void,
) -> ae::PF_Err;

struct Guest {
    _lib: libloading::Library,
    entry: EffectMainFn,
    params: Vec<ae::PF_ParamDef>,
    global_data: ae::PF_Handle,
    sequence_data: ae::PF_Handle,
    sequence_ready: bool,
    name: String,
    error: CString,
}

impl Guest {
    unsafe fn in_data(&mut self, width: i32, height: i32, frame: f64, fps: f64) -> ae::PF_InData {
        let mut ind: ae::PF_InData = std::mem::zeroed();
        ind.inter = ae::PF_InteractCallbacks {
            add_param: Some(cb_add_param),
            ..std::mem::zeroed()
        };
        ind.utils = Box::leak(Box::new(ae::PF_UtilCallbacks {
            iterate: Some(cb_iterate8),
            ..std::mem::zeroed()
        }));
        ind.pica_basicP = &SP_BASIC as *const _ as *mut _;
        ind.effect_ref = self as *mut Guest as ae::PF_ProgPtr;
        // 'FXTC': the After Effects application id. The one host quirk the
        // fleet's own plugins key off (Premiere is BGRA) keys off 'PrMr', so
        // reporting FXTC gets the ARGB path — which is what PF worlds are.
        ind.appl_id = i32::from_be_bytes(*b"FXTC");
        ind.version = ae::PF_SpecVersion { major: 13, minor: 28 };
        ind.width = width;
        ind.height = height;
        ind.extent_hint = ae::PF_Rect { left: 0, top: 0, right: width, bottom: height };
        ind.pixel_aspect_ratio = ae::PF_RationalScale { num: 1, den: 1 };
        ind.time_scale = (fps.max(1.0) * 1000.0) as u32;
        ind.time_step = 1000;
        ind.local_time_step = 1000;
        ind.total_time = 1000 * 100000;
        ind.current_time = (frame * 1000.0) as i32;
        ind.field = ae::PF_Field_FRAME as i32;
        ind.global_data = self.global_data;
        ind.sequence_data = self.sequence_data;
        ind
    }

    unsafe fn call(
        &mut self,
        cmd: ae::PF_Cmd,
        ind: &mut ae::PF_InData,
        outd: &mut ae::PF_OutData,
        params: &mut [*mut ae::PF_ParamDef],
        output: *mut ae::PF_LayerDef,
    ) -> ae::PF_Err {
        let params_ptr = if params.is_empty() {
            std::ptr::null_mut()
        } else {
            // The AE convention: EffectMain receives PF_ParamDef*[] — an array
            // of pointers — despite the prototype's single-pointer shape.
            params.as_mut_ptr() as *mut ae::PF_ParamDef
        };
        let entry = self.entry;
        entry(cmd, ind, outd, params_ptr, output, std::ptr::null_mut())
    }
}

fn world_from_rgba(rgba: &[u8], width: i32, height: i32, buf: &mut Vec<ae::PF_Pixel>) -> ae::PF_EffectWorld {
    buf.clear();
    buf.reserve((width * height) as usize);
    for px in rgba.chunks_exact(4) {
        buf.push(ae::PF_Pixel { alpha: px[3], red: px[0], green: px[1], blue: px[2] });
    }
    unsafe {
        let mut w: ae::PF_EffectWorld = std::mem::zeroed();
        w.width = width;
        w.height = height;
        w.rowbytes = width * std::mem::size_of::<ae::PF_Pixel>() as i32;
        w.data = buf.as_mut_ptr() as *mut _;
        w.extent_hint = ae::PF_Rect { left: 0, top: 0, right: width, bottom: height };
        w
    }
}

// ---------------------------------------------------------------------------
// C ABI.
// ---------------------------------------------------------------------------

fn set_error(guest: &mut Guest, message: &str) {
    guest.error = CString::new(message).unwrap_or_default();
}

/// Open a .plugin bundle (or bare binary), run GLOBAL_SETUP and PARAMS_SETUP,
/// and return an opaque guest. Null on failure, with the reason on stderr.
#[no_mangle]
pub unsafe extern "C" fn aeg_open(path: *const c_char) -> *mut Guest {
    let path = CStr::from_ptr(path).to_string_lossy().into_owned();

    // Bundle -> Contents/MacOS/<first binary>.
    let binary = {
        let macos = std::path::Path::new(&path).join("Contents").join("MacOS");
        if macos.is_dir() {
            match std::fs::read_dir(&macos).ok().and_then(|mut d| d.next()).and_then(|e| e.ok()) {
                Some(e) => e.path(),
                None => {
                    eprintln!("aeguest: nothing inside {}", macos.display());
                    return std::ptr::null_mut();
                }
            }
        } else {
            std::path::PathBuf::from(&path)
        }
    };

    let lib = match libloading::Library::new(&binary) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("aeguest: dlopen failed: {e}");
            return std::ptr::null_mut();
        }
    };
    let entry: EffectMainFn = match lib.get::<EffectMainFn>(b"EffectMain\0") {
        Ok(sym) => *sym,
        Err(_) => match lib.get::<EffectMainFn>(b"main\0") {
            Ok(sym) => *sym,
            Err(e) => {
                eprintln!("aeguest: no EffectMain export: {e}");
                return std::ptr::null_mut();
            }
        },
    };

    let guest = Box::into_raw(Box::new(Guest {
        _lib: lib,
        entry,
        params: Vec::new(),
        global_data: std::ptr::null_mut(),
        sequence_data: std::ptr::null_mut(),
        sequence_ready: false,
        name: String::new(),
        error: CString::default(),
    }));

    let g = &mut *guest;
    let mut ind = g.in_data(64, 64, 0.0, 25.0);
    let mut outd: ae::PF_OutData = std::mem::zeroed();

    let err = g.call(ae::PF_Cmd_GLOBAL_SETUP as ae::PF_Cmd, &mut ind, &mut outd, &mut [], std::ptr::null_mut());
    if err != ae::PF_Err_NONE as ae::PF_Err {
        eprintln!("aeguest: GLOBAL_SETUP failed ({err})");
        drop(Box::from_raw(guest));
        return std::ptr::null_mut();
    }
    g.global_data = outd.global_data;

    let mut ind = g.in_data(64, 64, 0.0, 25.0);
    let mut outd: ae::PF_OutData = std::mem::zeroed();
    let err = g.call(ae::PF_Cmd_PARAMS_SETUP as ae::PF_Cmd, &mut ind, &mut outd, &mut [], std::ptr::null_mut());
    if err != ae::PF_Err_NONE as ae::PF_Err {
        eprintln!("aeguest: PARAMS_SETUP failed ({err})");
        drop(Box::from_raw(guest));
        return std::ptr::null_mut();
    }

    guest
}

/// Parameter metadata as JSON, matching the bridge's manifest vocabulary.
/// Caller frees with aeg_free_string.
#[no_mangle]
pub unsafe extern "C" fn aeg_describe_json(guest: *mut Guest) -> *mut c_char {
    let g = &*guest;

    fn esc(s: &str) -> String {
        s.replace('\\', "\\\\").replace('"', "\\\"")
    }

    let mut out = String::from("{\n  \"params\": [\n");
    let mut first = true;
    for (i, p) in g.params.iter().enumerate() {
        let name = CStr::from_ptr(p.name.as_ptr()).to_string_lossy();
        let ptype = p.param_type;

        let (kind, def, min, max, options): (&str, f64, f64, f64, Vec<String>) =
            match ptype {
                ae::PF_Param_FLOAT_SLIDER => {
                    let fs = p.u.fs_d;
                    ("float", fs.value, fs.valid_min as f64, fs.valid_max as f64, vec![])
                }
                ae::PF_Param_SLIDER => {
                    let sd = p.u.sd;
                    ("float", sd.value as f64, sd.valid_min as f64, sd.valid_max as f64, vec![])
                }
                ae::PF_Param_CHECKBOX => {
                    let bd = p.u.bd;
                    ("bool", bd.value as f64, 0.0, 1.0, vec![])
                }
                ae::PF_Param_POPUP => {
                    let pd = p.u.pd;
                    // Popup options arrive as one string, pipe-separated.
                    let items = if pd.u.namesptr.is_null() {
                        vec![]
                    } else {
                        CStr::from_ptr(pd.u.namesptr)
                            .to_string_lossy()
                            .split('|')
                            .map(|s| s.trim().to_string())
                            .collect()
                    };
                    ("popup", (pd.value - 1) as f64, 0.0, (pd.num_choices - 1) as f64, items)
                }
                ae::PF_Param_COLOR => {
                    let cd = p.u.cd;
                    let packed = ((cd.value.red as u32) << 16)
                        | ((cd.value.green as u32) << 8)
                        | (cd.value.blue as u32);
                    ("color", packed as f64, 0.0, 16777215.0, vec![])
                }
                ae::PF_Param_ANGLE => {
                    let ad = p.u.ad;
                    ("float", ad.value as f64 / 65536.0, 0.0, 360.0, vec![])
                }
                _ => ("unsupported", 0.0, 0.0, 1.0, vec![]),
            };

        if !first {
            out += ",\n";
        }
        first = false;
        out += &format!(
            "    {{\"index\": {i}, \"name\": \"{}\", \"kind\": \"{kind}\", \"default\": {def}, \"min\": {min}, \"max\": {max}",
            esc(&name)
        );
        if !options.is_empty() {
            out += ", \"options\": [";
            for (n, opt) in options.iter().enumerate() {
                if n > 0 {
                    out += ",";
                }
                out += &format!("\"{}\"", esc(opt));
            }
            out += "]";
        }
        out += "}";
    }
    out += "\n  ]\n}\n";

    CString::new(out).unwrap_or_default().into_raw()
}

#[no_mangle]
pub unsafe extern "C" fn aeg_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

/// Set a parameter by its position in the described list (input layer is NOT
/// counted here; index 0 is the first real parameter).
#[no_mangle]
pub unsafe extern "C" fn aeg_set_param(guest: *mut Guest, index: i32, value: f64) {
    let g = &mut *guest;
    let Some(p) = g.params.get_mut(index as usize) else { return };
    match p.param_type {
        ae::PF_Param_FLOAT_SLIDER => p.u.fs_d.value = value,
        ae::PF_Param_SLIDER => p.u.sd.value = value as i32,
        ae::PF_Param_CHECKBOX => p.u.bd.value = (value >= 0.5) as i32,
        ae::PF_Param_POPUP => p.u.pd.value = value as i32 + 1,
        ae::PF_Param_ANGLE => p.u.ad.value = (value * 65536.0) as i32,
        ae::PF_Param_COLOR => {
            let packed = value as u32;
            p.u.cd.value.red = ((packed >> 16) & 0xff) as u8;
            p.u.cd.value.green = ((packed >> 8) & 0xff) as u8;
            p.u.cd.value.blue = (packed & 0xff) as u8;
            p.u.cd.value.alpha = 255;
        }
        _ => {}
    }
}

/// Render one frame. `in_rgba` may be null (the input layer is then blank).
/// Frames are width*height*4 RGBA8, row 0 first.
#[no_mangle]
pub unsafe extern "C" fn aeg_render(
    guest: *mut Guest,
    in_rgba: *const u8,
    out_rgba: *mut u8,
    width: i32,
    height: i32,
    frame: f64,
    fps: f64,
) -> i32 {
    let g = &mut *guest;
    let n = (width * height) as usize * 4;

    let in_slice: &[u8] = if in_rgba.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(in_rgba, n)
    };

    let mut src_buf: Vec<ae::PF_Pixel> = Vec::new();
    let mut src_world = if in_slice.is_empty() {
        let blank = vec![0u8; n];
        world_from_rgba(&blank, width, height, &mut src_buf)
    } else {
        world_from_rgba(in_slice, width, height, &mut src_buf)
    };

    let mut dst_buf: Vec<ae::PF_Pixel> = vec![std::mem::zeroed(); (width * height) as usize];
    let mut dst_world: ae::PF_EffectWorld = std::mem::zeroed();
    dst_world.width = width;
    dst_world.height = height;
    dst_world.rowbytes = width * std::mem::size_of::<ae::PF_Pixel>() as i32;
    dst_world.data = dst_buf.as_mut_ptr() as *mut _;
    dst_world.extent_hint = ae::PF_Rect { left: 0, top: 0, right: width, bottom: height };

    // SEQUENCE_SETUP once per guest: some effects allocate per-sequence state.
    if !g.sequence_ready {
        let mut ind = g.in_data(width, height, frame, fps);
        let mut outd: ae::PF_OutData = std::mem::zeroed();
        let err = g.call(ae::PF_Cmd_SEQUENCE_SETUP as ae::PF_Cmd, &mut ind, &mut outd, &mut [], std::ptr::null_mut());
        if err != ae::PF_Err_NONE as ae::PF_Err {
            set_error(g, &format!("SEQUENCE_SETUP failed ({err})"));
            return 1;
        }
        g.sequence_data = outd.sequence_data;
        g.sequence_ready = true;
    }

    // params[0] is the input layer, by AE convention.
    let mut layer_param: ae::PF_ParamDef = std::mem::zeroed();
    layer_param.param_type = ae::PF_Param_LAYER as i32;
    layer_param.u.ld = src_world;

    let mut param_ptrs: Vec<*mut ae::PF_ParamDef> = Vec::with_capacity(1 + g.params.len());
    param_ptrs.push(&mut layer_param);
    // Split borrow: the pointers into g.params stay valid for the call.
    let params_raw: *mut Vec<ae::PF_ParamDef> = &mut g.params;
    for p in (*params_raw).iter_mut() {
        param_ptrs.push(p);
    }

    let mut ind = g.in_data(width, height, frame, fps);
    let mut outd: ae::PF_OutData = std::mem::zeroed();

    let err = g.call(ae::PF_Cmd_RENDER as ae::PF_Cmd, &mut ind, &mut outd, &mut param_ptrs, &mut dst_world);
    if err != ae::PF_Err_NONE as ae::PF_Err {
        set_error(g, &format!("RENDER failed ({err})"));
        return 1;
    }

    let out = std::slice::from_raw_parts_mut(out_rgba, n);
    for (i, px) in dst_buf.iter().enumerate() {
        out[i * 4] = px.red;
        out[i * 4 + 1] = px.green;
        out[i * 4 + 2] = px.blue;
        out[i * 4 + 3] = px.alpha;
    }
    let _ = src_world.data; // keep src alive through the call
    0
}

#[no_mangle]
pub unsafe extern "C" fn aeg_last_error(guest: *mut Guest) -> *const c_char {
    (*guest).error.as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn aeg_param_count(guest: *mut Guest) -> i32 {
    (*guest).params.len() as i32
}

#[no_mangle]
pub unsafe extern "C" fn aeg_close(guest: *mut Guest) {
    // The library handle is dropped with the Guest; libloading unloads it.
    // Guests with exit-time destructors survive because the process is not
    // exiting — this is mid-life teardown, the safe kind.
    if !guest.is_null() {
        drop(Box::from_raw(guest));
    }
}
