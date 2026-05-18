// EXPECT: 0
// _Complex / __complex__ — C99/C11 §6.2.5/11 complex types (GCC
// extension in C++). Sea-front previously lexed these to TK_KW_VOID
// which corrupted declarations like '_Complex int' into 'void int'.
// Slice 1 makes them a real type-specifier modifier that round-trips
// to the C output; semantic computation on complex values isn't
// modelled (sea-front isn't a math compiler).
//
// libstdc++ <complex> uses '__complex__ float' in function parameter
// types — covered by the parser shape change below.

_Complex int g;

float consume(__complex__ float z) { (void)z; return 0.0f; }

int main() {
    _Complex int local;
    local = 0;
    g = 0;
    (void)consume(0.0f);
    return 0;
}
