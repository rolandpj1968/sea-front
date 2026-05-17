// EXPECT: 0
// A block with a local class object (needing destruction) inside a
// non-loop scope must NOT emit __SF_CHAIN_BREAK / __SF_CHAIN_CONT
// goto-chains. Those macros expand to gotos whose target labels
// only exist inside a loop body — outside one they reference an
// undefined symbol and cc rejects.
//
// Surfaced by gcc 4.8 g++.dg/eh/ctor1 (and 8 sibling cases): a
// 'try { Bar f; }' block had a non-trivial dtor on f, and the
// goto-chain emit blindly walked the top of g_cf.live for the
// break/cont target — which was the try block's CL_TRY label
// (treated as a __SF_cleanup_N name) — producing
// '__SF_CHAIN_BREAK(__SF_cleanup_0)' with no such label declared.

struct Trace { int *log; int v; ~Trace() { *log += v; } };

int main() {
    int log = 0;
    {
        Trace t = { &log, 7 };
        // No loop here. The cleanup chain MUST emit only the throw
        // and return chains; break/cont chains would reference labels
        // that do not exist in this scope.
    }
    return log != 7;   // Trace dtor must have run exactly once.
}
