// SPDX-License-Identifier: GPL-2.0
//
// Build script for scx_cake - compiles BPF code and generates bindings

fn main() {
    // Force BPF v4 instruction set for better code density and 32-bit ALU support
    let mut cflags = String::from("-O3 -mcpu=v4");
    if std::env::var("CARGO_FEATURE_DEBUG_STATS").is_ok() {
        cflags.push_str(" -DCAKE_DEBUG_STATS");
    }
    std::env::set_var("BPF_EXTRA_CFLAGS_PRE_INCL", &cflags);

    scx_cargo::BpfBuilder::new()
        .unwrap()
        .enable_intf("src/bpf/intf.h", "bpf_intf.rs")
        .enable_skel("src/bpf/cake.bpf.c", "bpf")
        .compile_link_gen()
        .unwrap();
}
