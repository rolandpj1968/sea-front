typedef int MyInt;
struct Point { int x; int y; };

int main() {
    MyInt x = (MyInt)42;
    int s1 = sizeof(MyInt);
    int s2 = sizeof(Point);
    int *p = (int *)0;
    return x + s1 + s2;
}
