// EXPECT: 0
// `new T()` for a POD T (no user ctor) value-initializes T — zero-
// init for arithmetic / pointer subobjects. N4659 §8.5.1/8
// [dcl.init]. Sea-front emits a memset over the allocation after
// the malloc call returns.

extern "C" void free(void *);

struct POD {
    int    a;
    double b;
    char   c[8];
};

int main() {
    // Scalar new with value-init.
    POD *p = new POD();
    if (p->a   != 0)            return 1;
    if (p->b   != 0.0)          return 2;
    for (int i = 0; i < 8; i++)
        if (p->c[i] != 0)       return 3;
    free(p);

    // Array new with value-init — same shape but N elements.
    POD *arr = new POD[5]();
    for (int i = 0; i < 5; i++) {
        if (arr[i].a != 0)      return 4;
        if (arr[i].b != 0.0)    return 5;
    }
    free(arr);
    return 0;
}
