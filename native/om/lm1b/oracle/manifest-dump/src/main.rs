// SPDX-License-Identifier: AGPL-3.0-or-later
// Reference-oracle tool for native/om/lm1b: print the pinned manifest of a GGUF file in the
// exact text form lm1b.om rebuilds at load, using the aienos-infer parser (an independent reader).
use aienos_infer::{GgmlType, Gguf, Value};
fn show(s: &str) -> String {
    let ok = s.len() <= 40 && s.bytes().all(|b| (32..=126).contains(&b) && b != b'"' && b != b'\\');
    if ok { format!("{} {}", s.len(), s) } else { format!("{}", s.len()) }
}
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let bytes = std::fs::read(&a[1]).expect("read");
    let g = Gguf::parse(&bytes).expect("gguf");
    println!("gguf {} {} {}", g.version, g.tensors().len(), g.kvs().len());
    for (k, v) in g.kvs() {
        let s = match *v {
            Value::U8(x) => format!("0 {x}"),
            Value::I8(x) => format!("1 {x}"),
            Value::U16(x) => format!("2 {x}"),
            Value::I16(x) => format!("3 {x}"),
            Value::U32(x) => format!("4 {x}"),
            Value::I32(x) => format!("5 {x}"),
            Value::F32(x) => format!("6 {}", x.to_bits()),
            Value::Bool(x) => format!("7 {}", x as u8),
            Value::Str(x) => format!("8 {}", show(x)),
            Value::Array(x) => {
                let e = match format!("{:?}", x.elem).as_str() {
                    "U8" => 0, "I8" => 1, "U16" => 2, "I16" => 3, "U32" => 4, "I32" => 5, "F32" => 6,
                    "Bool" => 7, "Str" => 8, "Array" => 9, "U64" => 10, "I64" => 11, "F64" => 12,
                    o => panic!("elem {o}"),
                };
                format!("9 {e} {}", x.len())
            }
            Value::U64(x) => format!("10 {x}"),
            Value::I64(x) => format!("11 {x}"),
            Value::F64(x) => format!("12 {}", x.to_bits()),
        };
        println!("kv {} {}", show(k), s);
    }
    for t in g.tensors() {
        let code = (0u32..64).find(|c| GgmlType::from_code(*c).ok() == Some(t.ty)).expect("code");
        assert!(t.n_dims <= 2);
        println!("t {} {} {} {} {} {}", t.name, code, t.n_dims, t.dims[0], t.dims[1], t.offset);
    }
    println!("data {}", g.data_start);
}
