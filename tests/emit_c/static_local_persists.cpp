// EXPECT: 0
// N4659 §10.1.1 [dcl.stc]: 'static' on a function-local variable
// gives it static storage duration — the value persists across
// calls, and zero-initialization runs once. Sea-front's block-
// scope ND_VAR_DECL emit was calling emit_var_decl_inner directly
// without emitting storage_flags, so 'static' was dropped on
// function-locals. The variable became an auto local, lost its
// value between calls, and (worse) wasn't zero-initialized — its
// stack-garbage starting value silently broke once-only init
// guards like
//
//   static rtx queue_head;
//   if (queue_head == 0) initialize_iterators();
//
// in gcc 4.8 read-rtl.c — leaving codes.iterators NULL and
// causing genpreds / genconditions to segfault on first
// htab_find call.
int next_id() {
    static int counter = 0;
    return ++counter;
}

int main() {
    int a = next_id();
    int b = next_id();
    int c = next_id();
    return (a == 1 && b == 2 && c == 3) ? 0 : 1;
}
