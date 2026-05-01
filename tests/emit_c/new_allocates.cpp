// EXPECT: 7
// 'new T' must allocate storage; reading/writing through the
// resulting pointer must not crash. Previously sea-front lowered
// 'new T' to '(T*)0' (a NULL cast), so any subsequent dereference
// segfaulted at runtime.
//
// Lowering: 'new T' → '(T *)malloc(sizeof(T))'. Trivial-ctor /
// POD types only — non-trivial ctor running on the fresh storage
// is deferred (TODO seafront#new-ctor).

struct Box { int v; };

int main() {
    Box *b = new Box;
    b->v = 7;
    int r = b->v;
    /* leak intentionally — bootstrap test, no destructor needed */
    return r;
}
