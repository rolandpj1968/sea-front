// EXPECT: 4
// Slice D-cond: temp in a for-loop condition. The for-loop is
// lowered to a while(1) form because the for's cond slot can't
// hold a mini-block. Init runs once in a wrapping block; inc
// runs at the bottom of each iteration.
//
// Lowered:
//   {
//       int i = 0;
//       while (1) {
//           int __SF_cond_0;
//           { temp + assign + cleanup }
//           if (!__SF_cond_0) break;
//           { body }
//           i = (i + 1);
//       }
//   }
//
// i = 0,1,2 enter loop body (3 successful tests, 3 ~T calls),
// i = 3 fails (4th ~T call), terminate. g = 4.
//
// Init/inc temps are NOT yet handled by Slice D-cond — only
// the cond. Documented limitation.
int g = 0;

struct T {
    int v;
    T(int x) { v = x; }
    ~T() { g = g + 1; }
};

int main() {
    for (int i = 0; T(i).v < 3; i = i + 1) {
        /* body */
    }
    return g;
}
