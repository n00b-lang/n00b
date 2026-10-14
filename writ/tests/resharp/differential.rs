use resharp_writ::*;
use std::ptr::{null, null_mut};

const PATTERNS: &[&str] = &[
    "a+",
    "[0-9]+",
    "foo|bar",
    "",
    "x*",
    "(ab)+c",
    "[a-z]+@[a-z]+\\.com",
    "^start",
    "end$",
    "\\d{2,4}",
    "(",
    "[",
    "a{3,1}",
    "é+",
];

const INPUTS: &[&[u8]] = &[
    b"",
    b"aaa",
    b"foo bar baz 123 4567",
    b"start and end",
    b"mail me@host.com now",
    b"ababc abc xxx",
    "caf\u{e9}\u{e9}".as_bytes(),
    b"\xff\xfe raw bytes 99",
];

fn last_error() -> String {
    unsafe {
        let total = writ_resharp_last_error(null_mut(), 0);
        let mut text = vec![0u8; total];
        writ_resharp_last_error(text.as_mut_ptr(), total);
        String::from_utf8(text).unwrap()
    }
}

fn compile(pattern: &str) -> Result<*mut writ_resharp_regex, String> {
    let mut regex = null_mut();
    let status = unsafe { writ_resharp_regex_new(pattern.as_ptr(), pattern.len(), &mut regex) };
    if status == STATUS_OK {
        Ok(regex)
    } else {
        assert_eq!(status, STATUS_ERROR);
        Err(last_error())
    }
}

fn find_all(regex: *mut writ_resharp_regex, input: &[u8]) -> Result<Vec<(usize, usize)>, String> {
    let mut total = 0usize;
    let status = unsafe { writ_resharp_regex_find_all(regex, input.as_ptr(), input.len(), null_mut(), 0, &mut total) };
    if status != STATUS_OK {
        return Err(last_error());
    }
    let mut spans = vec![Match { start: 0, end: 0 }; total];
    let mut again = 0usize;
    let status =
        unsafe { writ_resharp_regex_find_all(regex, input.as_ptr(), input.len(), spans.as_mut_ptr(), total, &mut again) };
    assert_eq!(status, STATUS_OK);
    assert_eq!(again, total);
    Ok(spans.iter().map(|span| (span.start, span.end)).collect())
}

fn is_match(regex: *mut writ_resharp_regex, input: &[u8]) -> Result<bool, String> {
    let mut found = false;
    match unsafe { writ_resharp_regex_is_match(regex, input.as_ptr(), input.len(), &mut found) } {
        STATUS_OK => Ok(found),
        _ => Err(last_error()),
    }
}

fn find_anchored(regex: *mut writ_resharp_regex, input: &[u8]) -> Result<Option<(usize, usize)>, String> {
    let mut span = Match { start: 0, end: 0 };
    match unsafe { writ_resharp_regex_find_anchored(regex, input.as_ptr(), input.len(), &mut span) } {
        STATUS_OK => Ok(Some((span.start, span.end))),
        STATUS_NONE => Ok(None),
        _ => Err(last_error()),
    }
}

fn corpus<T: Copy>(items: &[T]) -> &[T] {
    if cfg!(miri) {
        &items[..2]
    } else {
        items
    }
}

#[test]
fn transport_matches_the_library() {
    for pattern in corpus(PATTERNS) {
        let direct = resharp::Regex::new(pattern);
        let through = compile(pattern);
        match (direct, through) {
            (Err(error), Err(text)) => assert_eq!(error.to_string(), text, "{pattern}"),
            (Ok(regex), Ok(handle)) => {
                for input in corpus(INPUTS) {
                    let spans = regex.find_all(input).map(|all| all.iter().map(|m| (m.start, m.end)).collect());
                    assert_eq!(spans.map_err(|e| e.to_string()), find_all(handle, input), "{pattern}");
                    assert_eq!(regex.is_match(input).map_err(|e| e.to_string()), is_match(handle, input));
                    let anchored = regex.find_anchored(input).map(|m| m.map(|m| (m.start, m.end)));
                    assert_eq!(anchored.map_err(|e| e.to_string()), find_anchored(handle, input));
                }
                unsafe { writ_resharp_regex_free(handle) };
            }
            (direct, through) => panic!("{pattern}: {:?} vs {:?}", direct.is_ok(), through.is_ok()),
        }
    }
}

#[test]
fn empty_inputs_cross_as_null() {
    let regex = compile("a*").unwrap();
    let mut found = false;
    assert_eq!(unsafe { writ_resharp_regex_is_match(regex, null(), 0, &mut found) }, STATUS_OK);
    assert!(found);
    let mut total = 7usize;
    assert_eq!(unsafe { writ_resharp_regex_find_all(regex, null(), 0, null_mut(), 0, &mut total) }, STATUS_OK);
    assert_eq!(total, resharp::Regex::new("a*").unwrap().find_all(b"").unwrap().len());
    let mut empty = null_mut();
    assert_eq!(unsafe { writ_resharp_regex_new(null(), 0, &mut empty) }, STATUS_OK);
    unsafe {
        writ_resharp_regex_free(empty);
        writ_resharp_regex_free(regex);
        writ_resharp_regex_free(null_mut());
    }
}

#[test]
fn zero_capacity_reports_the_total() {
    let regex = compile("b").unwrap();
    let mut total = 0usize;
    let input = b"abcb";
    let status = unsafe { writ_resharp_regex_find_all(regex, input.as_ptr(), input.len(), null_mut(), 0, &mut total) };
    assert_eq!(status, STATUS_OK);
    assert_eq!(total, 2);
    let mut one = [Match { start: 0, end: 0 }];
    let status = unsafe { writ_resharp_regex_find_all(regex, input.as_ptr(), input.len(), one.as_mut_ptr(), 1, &mut total) };
    assert_eq!(status, STATUS_OK);
    assert_eq!((one[0].start, one[0].end, total), (1, 2, 2));
    unsafe { writ_resharp_regex_free(regex) };
}

#[test]
fn errors_and_options() {
    let error = compile("(").unwrap_err();
    assert_eq!(Some(error), resharp::Regex::new("(").err().map(|error| error.to_string()));
    let invalid = [0xffu8];
    let mut regex = null_mut();
    assert_eq!(unsafe { writ_resharp_regex_new(invalid.as_ptr(), 1, &mut regex) }, STATUS_ERROR);
    assert!(!last_error().is_empty());
    let mut options = null_mut();
    let mut folded = null_mut();
    unsafe {
        assert_eq!(writ_resharp_regex_options_default(&mut options), STATUS_OK);
        assert_eq!(writ_resharp_regex_options_case_insensitive(options, true, &mut folded), STATUS_OK);
        assert_eq!(writ_resharp_regex_options_unicode(folded, 9, &mut options), STATUS_ERROR);
        assert_eq!(writ_resharp_regex_options_default(&mut options), STATUS_OK);
        assert_eq!(writ_resharp_regex_options_unicode(options, 0, &mut folded), STATUS_OK);
        assert_eq!(writ_resharp_regex_with_options(b"x".as_ptr(), 1, folded, &mut regex), STATUS_OK);
        writ_resharp_regex_free(regex);
    }
}
