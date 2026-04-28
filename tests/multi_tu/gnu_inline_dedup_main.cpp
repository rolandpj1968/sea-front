// EXPECT: 8
// Companion to gnu_inline_dedup_a.cpp. Only declares f and
// calls it. The cross-TU link succeeds only when _a.cpp's .o
// exports the strong f() symbol. Without 248271c, _a.cpp's .o
// has only the gnu_inline hint and 'undefined reference to f'
// at link.

extern int f(int x);

int main() {
    return f(7);   // 7 + 1 = 8
}
