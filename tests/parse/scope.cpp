typedef int MyType;
int main() {
    MyType x = 1;
    {
        typedef double MyType;
        MyType y = 2.0;
    }
    MyType z = 3;
    return x + z;
}
