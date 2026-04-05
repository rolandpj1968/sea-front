typedef int MyInt;
MyInt x = 42;
MyInt add(MyInt a, MyInt b) { return a + b; }
int main() { MyInt r = add(x, 10); return r; }
