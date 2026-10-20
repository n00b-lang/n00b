// hi
use std::fmt;
/* nested /* comment */ here */
fn main() {
    let x: u32 = 0xff_u32;
    let s = r#"raw "quoted" string"#;
    let c = b'x';
    let v: Vec<i32> = vec![1, 2, 3];
    println!("{} {} {:?}", x, s, v);
}
