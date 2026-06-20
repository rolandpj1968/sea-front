// EXPECT: 0
// N4659 §17.3.2/1 [temp.arg.nontype]: a function-pointer NTTP arg
// may be a constant expression that designates a function with
// external linkage. Both the bare function name (implicit
// function-to-pointer conversion) and the explicit address-of form
// `&fn` should be accepted.
//
// nttp_arg_to_literal_token previously only recognised bare-name
// (ND_IDENT). The `&fn` form (ND_UNARY with TK_AMP) fell through,
// the param bound as "unknown", and the cloned body emitted an
// unresolved `F`. The address-of unwrap routes the ND_UNARY operand
// back through the ident path so the bare-name binding kicks in.
//
// Caveat: the legacy mangler doesn't yet discriminate by NTTP value
// for function-pointer args, so distinct instantiations
// (call_with<&fa> vs call_with<&fb>) collide. This test verifies
// the SHAPE — that the body emits `F(v)` resolved to `fn(v)` —
// not yet the mangle. Multi-instantiation coverage waits on the
// funcptr-NTTP Itanium mangle slice.

extern "C" void abort();

int square(int x) { return x * x; }

template<int (*F)(int)> int call_with(int v) { return F(v); }

int main() {
    if (call_with<&square>(5) != 25) abort();
    return 0;
}
