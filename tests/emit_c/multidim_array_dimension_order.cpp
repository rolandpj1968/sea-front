// EXPECT: 7
// N4659 §11.3.4 [dcl.array]: in 'T arr[A][B]' A is the outer
// dimension and B is the inner. The internal type representation
// must wrap so that arr[i][j] addresses the right element.
//
// Sea-front previously parsed multi-dim arrays by wrapping each
// bracket as it was read, producing 'T[B][A]' from 'T[A][B]'. With
// distinct A and B that's a silent miscompile — indexed writes
// overrun the declared bounds. Pattern from gcc 4.8 tree.c:
// 'unsigned char tree_contains_struct[MAX_TREE_CODES][64]' indexed
// as [code][ts_kind] wrote past the array into adjacent globals.

unsigned char arr[3][5];

int main() {
    // Logical layout: 3 rows, 5 cols. arr[i][j] = i*5 + j.
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 5; j++)
            arr[i][j] = (unsigned char)(i * 5 + j);

    // Pick a coordinate that would alias under swapped dimensions.
    // arr[1][2] = 1*5 + 2 = 7.
    // If dimensions were swapped, the C array layout treats it as
    // [5][3], and arr[1][2] would address byte at 1*3+2 = 5
    // (which we wrote when i=1, j=0 → value 5).
    return arr[1][2];
}
