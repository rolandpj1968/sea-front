// EXPECT: 7
// EH slice 4: 'catch (...)' catches any in-flight throw — N4659
// §18.3 [except.handle]/5. The catch-all has no exception-
// declaration, so no payload binding; the handler body just runs.
//
// Verifies: a thrown int reaches the catch-all handler when no
// typed catch precedes it. The first handler is a typed 'catch (long)'
// — its primitive-typeinfo pointer compare is satisfied for slice 4
// (a single shared __sf_typeinfo_int placeholder is used for all
// integral primitives until per-type instances ship), but the catch-
// all is the realistic-shape failsafe in libstdc++ patterns.

int main() {
    try {
        throw 7;
    } catch (...) {
        return 7;
    }
    return -1;
}
