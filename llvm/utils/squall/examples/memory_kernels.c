/* Aligned cell-memory ABI exercises. Inputs stay within the declared arrays. */
int a[8] = {2, -3, 5, 7, 11, -13, 17, 19};
int b[8] = {23, 29, -31, 37, 41, 43, -47, 53};
int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};

int dot_impl(int *left, int *right, int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += left[i] * right[i];
    return sum;
}
int dot(int n) { return dot_impl(a, b, n); }
int reduction(int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += a[i];
    return sum;
}
int local_array(int n) {
    int values[8];
    for (int i = 0; i < 8; ++i) values[i] = i * 3 - n;
    return dot_impl(values, b, 8);
}
int matrix_product(void) {
    int result[2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            result[i][j] = 0;
            for (int k = 0; k < 3; ++k)
                result[i][j] += matrix[i][k] * matrix[j][k];
        }
    return result[0][0] + result[0][1] + result[1][0] + result[1][1];
}
int initialized_local(int n) {
    int values[4] = {3, -5, 7, 11};
    return values[n];
}
