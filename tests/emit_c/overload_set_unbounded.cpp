// EXPECT: 42
// Overload resolution used to cap the candidate set at
// MAX_OVLD_CANDS = 16 and silently drop overloads past the 16th.
// Sea-front now arena-allocates the candidate / ranks / spec-order
// arrays to fit the actual ncands, so 17+-way overload sets resolve
// correctly. Pattern modelled on libstdc++ iostream operator<< /
// std::max which routinely exceed 16 overloads in modern stdlibs.

int f(int v0) { return 100; }
int f(int v0, int v1) { return 200; }
int f(int v0, int v1, int v2) { return 300; }
int f(int v0, int v1, int v2, int v3) { return 400; }
int f(int v0, int v1, int v2, int v3, int v4) { return 500; }
int f(int v0, int v1, int v2, int v3, int v4, int v5) { return 600; }
int f(int v0, int v1, int v2, int v3, int v4, int v5, int v6) { return 700; }
int f(int v0, int v1, int v2, int v3, int v4, int v5, int v6, int v7) { return 800; }
int f(long a) { return 900; }
int f(long a, long b) { return 1000; }
int f(long a, long b, long c) { return 1100; }
int f(long a, long b, long c, long d) { return 1200; }
int f(double a) { return 1300; }
int f(double a, double b) { return 1400; }
int f(char a) { return 1500; }
int f(char a, char b) { return 1600; }
int f(char a, char b, char c) { return 1700; }  // 17th overload — used to disappear
int f(short a) { return 42; }                    // 18th overload — also dropped

int main() {
    return f((short)0);  // must select the short overload, not silently fall through
}
