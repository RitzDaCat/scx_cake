// SPDX-License-Identifier: GPL-2.0
//
// Build script for scx_cake - compiles BPF code and generates bindings

fn main() {
    scx_cargo::BpfBuilder::new()
        .unwrap()
        .enable_intf("src/bpf/intf.h", "bpf_intf.rs")
        .enable_skel("src/bpf/cake.bpf.c", "bpf")
        .compile_link_gen()
        .unwrap();
}
