struct S { int x; int y; };
int arr[10];
int f(int a, int b);

int main() {
    int a = arr[3];
    S s;
    int b = s.x;
    S *p = &s;
    int c = p->y;
    int d = f(1, 2);
    a++;
    a--;
    return a + b + c + d;
}
