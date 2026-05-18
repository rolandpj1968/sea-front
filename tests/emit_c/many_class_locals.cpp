// EXPECT: 42
// CLEANUP_LIVE_MAX = 64 used to silently drop class-typed locals
// past the 64th from the cleanup chain — their dtors never ran. The
// live stack is now a malloc-grown buffer that fits any nesting depth.
// Also exercises the temp-name pool: pre-fix, names wrapped at 64
// and a borrowed-pointer reuse would substitute one temp's name for
// another's emit.

int ctor_count = 0;
int dtor_count = 0;

struct D {
    int v;
    D() { ++ctor_count; v = ctor_count; }
    ~D() { ++dtor_count; }
};

int main() {
    /* 80 class-typed locals — past the previous 64 cap. */
    D d00; D d01; D d02; D d03; D d04; D d05; D d06; D d07;
    D d08; D d09; D d10; D d11; D d12; D d13; D d14; D d15;
    D d16; D d17; D d18; D d19; D d20; D d21; D d22; D d23;
    D d24; D d25; D d26; D d27; D d28; D d29; D d30; D d31;
    D d32; D d33; D d34; D d35; D d36; D d37; D d38; D d39;
    D d40; D d41; D d42; D d43; D d44; D d45; D d46; D d47;
    D d48; D d49; D d50; D d51; D d52; D d53; D d54; D d55;
    D d56; D d57; D d58; D d59; D d60; D d61; D d62; D d63;
    D d64; D d65; D d66; D d67; D d68; D d69; D d70; D d71;
    D d72; D d73; D d74; D d75; D d76; D d77; D d78; D d79;
    int sum = d42.v;
    /* d42 was the 43rd local; check ctor ran with right ordinal. */
    return (sum == 43 && ctor_count == 80) ? 42 : 1;
}
