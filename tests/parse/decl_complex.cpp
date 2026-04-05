int arr[10];
int f(int x);
int (*fp)(int, int);
typedef int *IntPtr;
IntPtr p;
typedef int (*Callback)(int);
typedef int A;
typedef A B;
B val = 42;
enum Color { RED, GREEN, BLUE };
Color c;
struct Foo;
Foo *fwd;
extern "C" { int ext_func(void); }
extern "C" int ext_func2(void);
