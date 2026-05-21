// EXPECT: 0
// Multi-versioned functions via `__attribute__((target("X")))` —
// gcc extension. Two same-name function defs with different target
// attributes coexist; calls within a target-bearing function bind
// to the matching version.
//
// Mirrors g++.dg/ext/mv3.C. Sea-front's machinery:
//   - The C-level emitted name carries a `__target_<X>` suffix so
//     gcc-as-C (which rejects multi-versioned same-name defs)
//     accepts the output.
//   - emit_free_func_symbol at call sites looks up the version
//     whose target matches the enclosing function's target.
//   - When the caller has no target, prefer the `target("default")`
//     version, then the single versioned form, then fall through.
//
// Guards:
//   - `main` is never suffixed (linker needs the bare entry-point).
//   - When the TU has an unsuffixed forward declaration of NAME,
//     suppress the suffix entirely — gcc treats the forward as the
//     "merged default" and the bare name is what the ifunc
//     dispatcher expects.

int __attribute__((target("sse"))) versioned() {
    return 1;
}

int __attribute__((target("popcnt"))) versioned() {
    return 0;
}

int __attribute__((target("popcnt"))) caller() {
    return versioned();
}

int main() {
    return caller();
}
