// EXPECT: 0
// `: arr()` mem-init for an ARRAY of non-class members value-
// initializes each element (zero-init for arithmetic types) —
// emits as a memset over the whole storage. N4659 §15.6.2/8
// [class.base.init] + §11.6/8 [dcl.init].

struct Holder {
    unsigned char buf[64];
    int           ints[8];
    Holder() : buf(), ints() {}
};

void *operator new(unsigned long, void *p) { return p; }

int main() {
    unsigned char storage[sizeof(Holder)];
    for (unsigned i = 0; i < sizeof(storage); i++) storage[i] = 0xAA;

    Holder *h = new (storage) Holder();
    for (int i = 0; i < 64; i++)
        if (h->buf[i] != 0)  return 1;
    for (int i = 0; i < 8; i++)
        if (h->ints[i] != 0) return 2;
    return 0;
}
