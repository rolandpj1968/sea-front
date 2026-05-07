// EXPECT: 7
// N4659 §16.3.3.2 [over.ics.rank] / §7.5 [conv.qual]: a free-function
// overload set differing only in pointee const-qualification —
//   f(T *)        ← non-const pointee
//   f(const T *)  ← const pointee
// must resolve a `T*` argument to the non-const overload (exact match)
// and a `const T*` argument to the const overload. Real-world hit:
// gcc 14 libcpp/init.cc
//   auto *last = linemap_check_ordinary(LINEMAPS_LAST_MAP(...));
//   last->to_line = 1;
// — sea-front used to pick the const overload and the assignment
// failed cc with "assignment of member in read-only object."
struct Box { int v; };

inline int label(Box *)       { return 1; }
inline int label(const Box *) { return 2; }

int main() {
    Box a;
    const Box c = { 0 };
    int s = 0;
    s += label(&a);        // T* → non-const overload returns 1
    s += label(&c) * 3;    // const T* → const overload returns 2; *3 = 6
    return s;              // 1 + 6 = 7
}
