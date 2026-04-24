// EXPECT: 42
// A struct defined in block scope must be emitted inside the block,
// not dropped as '/* stmt */'. C allows block-scope struct declarations
// (the type has local lifetime). The two-phase emit only walks top-level
// ND_CLASS_DEFs in PHASE_STRUCTS, so function-body struct defs are only
// reached via emit_stmt and must emit the body there.
//
// Pattern from gcc 4.8 calls.c emit_library_call_value_1 (~line 3588):
//   void foo() {
//     struct arg {
//       rtx value;
//       enum machine_mode mode;
//       ...
//     };
//     struct arg *argvec;
//   }
// Without a block-scope emit path, downstream uses (sizeof(struct arg),
// ptr->field) hit 'incomplete type' errors.

int main() {
    struct point {
        int x;
        int y;
    };
    struct point p;
    p.x = 40;
    p.y = 2;
    return p.x + p.y;
}
