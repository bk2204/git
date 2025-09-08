fn main() {
    println!("cargo::rustc-link-search=.");
    println!("cargo::rustc-link-search=reftable");
    println!("cargo::rustc-link-search=xdiff");
    println!("cargo::rustc-link-lib=git");
    println!("cargo::rustc-link-lib=reftable");
    println!("cargo::rustc-link-lib=z");
    println!("cargo::rustc-link-lib=xdiff");
}
