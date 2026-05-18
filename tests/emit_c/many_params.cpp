// EXPECT: 42
// FFSIG_MAX_PARAMS = 16 used to truncate the signature recorded in
// the free-function overload table to 16 params; calls to 17+-param
// functions could mis-dedup against unrelated signatures. The
// signature now sizes to the actual nparams.

int sum20(int a, int b, int c, int d, int e, int f, int g, int h,
          int i, int j, int k, int l, int m, int n, int o, int p,
          int q, int r, int s, int t) {
    return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q+r+s+t;
}

int main() {
    int x = sum20(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1);  /* = 20 */
    return (x == 20) ? 42 : 1;
}
