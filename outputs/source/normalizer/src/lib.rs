use std::ffi::{CStr,CString,c_char};
use normalizer_tr::{Normalizer,NormalizeOptions,AmbiguityPolicy,Hint,HintKind,SourceRange};
use std::sync::OnceLock;
static NORMALIZER:OnceLock<Option<Normalizer>>=OnceLock::new();
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ajan_normalize(input:*const c_char)->*mut c_char {
    if input.is_null(){return std::ptr::null_mut();}
    let result=std::panic::catch_unwind(||{
        let text=unsafe{CStr::from_ptr(input)}.to_str().ok()?;
        let normalizer=NORMALIZER.get_or_init(||Normalizer::new().ok()).as_ref()?;
        let mut options=NormalizeOptions::default();
        options.ambiguity_policy=AmbiguityPolicy::Reject;
        for (pattern,kind) in [(r"\b\d{4}-\d{2}-\d{2}\b",HintKind::Date),(r"\b\d{2}\.\d{2}\.\d{4}\b",HintKind::Date),(r"\b\d{1,2}:\d{2}\b",HintKind::Time),(r"\+90\s\d{3}\s\d{3}\s\d{2}\s\d{2}",HintKind::Telephone)] {
            let re=regex::Regex::new(pattern).ok()?;
            for m in re.find_iter(text){options.hints.push(Hint::new(SourceRange::new(m.start(),m.end()),kind));}
        }
        let output=match normalizer.normalize(text,&options){Ok(v)=>v,Err(e)=>{eprintln!("Normalizer: {e:?}");return None;}};
        if !output.complete(){return None;}
        CString::new(output.normalized_text()).ok()
    });
    match result {Ok(Some(s))=>s.into_raw(),_=>std::ptr::null_mut()}
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ajan_normalize_free(value:*mut c_char){if !value.is_null(){drop(unsafe{CString::from_raw(value)});}}
