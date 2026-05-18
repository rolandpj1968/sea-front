// EXPECT: 42
// REF_PARAM_CAP = 32 used to silently drop ref-param entries past
// the 32nd from g_ref_params. Bodies referencing the dropped names
// emitted as bare-name reads instead of '*name' derefs, producing
// pointer values where the function expected int values.
//
// The table is now a malloc-grown buffer sized to the function's
// actual param count. 40-arg test exceeds the previous cap.

int sum40(
    int &a0,  int &a1,  int &a2,  int &a3,  int &a4,
    int &a5,  int &a6,  int &a7,  int &a8,  int &a9,
    int &a10, int &a11, int &a12, int &a13, int &a14,
    int &a15, int &a16, int &a17, int &a18, int &a19,
    int &a20, int &a21, int &a22, int &a23, int &a24,
    int &a25, int &a26, int &a27, int &a28, int &a29,
    int &a30, int &a31, int &a32, int &a33, int &a34,
    int &a35, int &a36, int &a37, int &a38, int &a39)
{
    return a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15+a16+a17+a18+a19
         + a20+a21+a22+a23+a24+a25+a26+a27+a28+a29+a30+a31+a32+a33+a34+a35+a36+a37+a38+a39;
}

int main() {
    int v[40];
    for (int i = 0; i < 40; ++i) v[i] = 1;
    /* Expected sum is 40. Pre-fix, the 9-args-past-32 had their
     * derefs dropped, so their bare values (addresses) were summed,
     * producing a wildly-wrong total. */
    int s = sum40(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],
                  v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19],
                  v[20],v[21],v[22],v[23],v[24],v[25],v[26],v[27],v[28],v[29],
                  v[30],v[31],v[32],v[33],v[34],v[35],v[36],v[37],v[38],v[39]);
    return (s == 40) ? 42 : 1;
}
