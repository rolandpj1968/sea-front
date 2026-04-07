// EXPECT: 7
// Mixed-type arithmetic: long widens int, the truncating cast back to
// int gives 7. Without identifier resolution, sema couldn't know that
// 'a' is long and 'b' is int — it would leave the binary node's
// resolved_type NULL. With resolution, it picks long (the wider rank).
int main() {
    long a = 5;
    int b = 2;
    return (int)(a + b);
}
