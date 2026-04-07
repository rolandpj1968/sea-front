// EXPECT: 42
// Parameters resolved through prototype scope. Function parameter 'a'
// must be visible inside the body via sema's cur_scope walk.
int identity(int a) {
    return a;
}

int main() {
    int v = identity(42);
    return v;
}
