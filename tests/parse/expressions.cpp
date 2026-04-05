int f(int x);
int main() {
    int a = sizeof(int);
    int b = sizeof a;
    int c = !a;
    int d = ~b;
    int e = -c;
    int arr[10];
    arr[0] = 1;
    int r = f(a + b);
    int *p = &r;
    return *p;
}
