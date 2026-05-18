// EXPECT: 42
// Array-of-array declarators used to silently truncate past
// MAX_ARRAY_DIMS=8. Sea-front now accumulates dims through a Vec so
// any number works. Pattern is contrived (9-dim array) but exercises
// the unbounded growth.

int a[2][2][2][2][2][2][2][2][2] = {};

int main() {
    a[1][0][1][0][1][0][1][0][1] = 42;
    return a[1][0][1][0][1][0][1][0][1];
}
