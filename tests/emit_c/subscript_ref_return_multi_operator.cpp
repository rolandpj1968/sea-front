// EXPECT: 42
// A class with MULTIPLE operator overloads (==, !=, [], etc.) must
// have its subscript ref-return detection use the WINNING overload's
// ret type — not 'the first operator in the class_region'. Previously
// the ref-return lookup matched operator== (first operator in vec),
// which returns bool (by value), causing operator[]'s ref-return
// wrapping '(*...)' to be skipped. That left the caller trying to
// deref a 'T*' result as if it were T, or treat a 'T**' result as 'T*'.
// Pattern from gcc 4.8 vec.h on the 'bitmap_descriptors[...]->field'
// access.
struct Holder {
    int* slots[16];
    int  nslots;
    Holder() : nslots(0) {}

    // Multiple operators — operator[] is NOT first.
    bool operator==(const Holder& other) const { return nslots == other.nslots; }
    bool operator!=(const Holder& other) const { return !(*this == other); }

    // operator[] returns reference — access to pointer slot.
    int*& operator[](int i) { return slots[i]; }
};

int main() {
    Holder h;
    int x = 42;
    h.slots[0] = &x;
    h.nslots = 1;
    // Without the fix: h[0] doesn't deref the T**, '->' fails or
    // dereferences stale memory.
    int* p = h[0];
    return *p;  // 42
}
