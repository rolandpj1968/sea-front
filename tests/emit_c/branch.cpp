// EXPECT: 1
// Two function calls + arithmetic. max(7,11)=11, max(20,3)=20, 11+20-30=1.
int max(int a, int b) {
    if (a > b)
        return a;
    else
        return b;
}

int main() {
    int x = max(7, 11);
    int y = max(20, 3);
    return x + y - 30;
}
