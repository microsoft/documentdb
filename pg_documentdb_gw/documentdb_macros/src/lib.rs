extern crate proc_macro;

use proc_macro::TokenStream;

#[proc_macro]
pub fn documentdb_int_error_mapping(_item: TokenStream) -> TokenStream {
    let mut result = String::new();
    let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../pg_documentdb_core/include/utils/external_error_mapping.csv");
    let csv = std::fs::File::open(path).unwrap();
    let reader = std::io::BufReader::new(csv);

    result += "pub fn known_error_code(state: &SqlState) -> Option<i32> {
                match state.code() {";
    for line in std::io::BufRead::lines(reader).skip(1) {
        let line = line.unwrap();
        let parts: Vec<&str> = line.split(',').collect();
        result += &format!("\"{}\" => Some({}),", parts[1], parts[2]);
    }
    result += "_ => None
    }
    }";
    result.parse().unwrap()
}
