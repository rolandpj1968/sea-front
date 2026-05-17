// Demo: constructor / destructor chains and the goto-chain cleanup
// (N4659 §6.6.3 [stmt.return], §15.4 [class.dtor], §15.6 [class.init]).
//
// Sea-front lowers every function with locals-needing-destruction into
// a single-exit form: returns become   __SF_RETURN(v, lbl);   which sets
// a per-function __SF_retval, raises __SF_unwind, and jumps forward
// through a chain of __SF_cleanup_N labels — each label runs exactly
// one destructor and then chains to the next.
//
// The same machinery is reused for exception unwind (see 05_exceptions).

struct Tracer {
    int id;
    int *log;
    int *idx;
    Tracer(int i, int *L, int *X) : id(i), log(L), idx(X) {
        log[(*idx)++] = +i;          // +i on construction
    }
    ~Tracer() {
        log[(*idx)++] = -id;         // -i on destruction
    }
};

int main() {
    int log[16] = {0};
    int idx = 0;

    {
        Tracer a(1, log, &idx);
        Tracer b(2, log, &idx);
        {
            Tracer c(3, log, &idx);
            // ~c at end of inner block
        }
        // ~b, then ~a at end of outer block
    }
    // Expected log: +1 +2 +3 -3 -2 -1
    // Sum of magnitudes = 12, sum of values = 0; return idx (=6).
    return idx;
}
