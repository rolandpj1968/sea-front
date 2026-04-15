// EXPECT: 42
// Function pointer as struct member with aggregate init — the
// gcc libcpp pattern. Exercises grouped-declarator fix (ptr wraps
// function, not return type), ND_INIT_LIST aggregate init, and
// call-site fptr-vs-method disambiguation.
struct conversion {
    const char *from;
    int (*func)(int);
};

static int add_one(int x) { return x + 1; }

static const struct conversion convs[] = {
    { "a", add_one },
    { "b", add_one },
    { 0, 0 }
};

int main() {
    return convs[0].func(41);
}
