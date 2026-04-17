// EXPECT: 30
// Test: switch/case/default with fallthrough
int classify(int x) {
    int r = 0;
    switch (x) {
    case 1: r = 10; break;
    case 2: r = 20; break;
    case 3:
    case 4: r = 30; break;  // fallthrough from 3 to 4
    default: r = 99; break;
    }
    return r;
}

int main() {
    return classify(3); // 30
}
