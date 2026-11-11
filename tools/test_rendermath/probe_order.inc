// Included into test_rmath.cpp's child: finds, for the original 4x4 (0x4144D0) and 3x4 (0x416100) multiplies,
// which operand comes first in each lane's multiply and add (the first operand's NaN wins when both are NaN).
static const uint32_t kP1 = 0x7FC00111, kP2 = 0x7FC00222, kP3 = 0x7FC00333;

static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// variant 0: 0x4144D0 (out, A, B), 4 rows, B rows 0..3 from B
// variant 1: 0x416100 (out, B, A), 3 rows, B rows 0..2 from B, row 3 the constant 0 0 0 1
static void probe_orders(int variant) {
    const int rows = variant == 0 ? 4 : 3;
    uint32_t mul_a_first[4][4][4] = {}, add_out_first[4][4][4] = {};
    for (int r = 0; r < rows; ++r)
        for (int k = 0; k < 4; ++k)
            for (int l = 0; l < 4; ++l) {
                // multiply order: a[r][k] = P1, B[k][l] = P2 (B row 3 is constant in variant 1: skip)
                if (!(variant == 1 && k == 3)) {
                    float A[16], B[16], O[16];
                    for (int i = 0; i < 16; ++i) { A[i] = 1.0f + i; B[i] = 2.0f + i; O[i] = 0; }
                    std::memcpy(&A[r * 4 + k], &kP1, 4);
                    std::memcpy(&B[k * 4 + l], &kP2, 4);
                    if (variant == 0) reinterpret_cast<F3>(0x4144D0)(O, A, B);
                    else reinterpret_cast<F3>(0x416100)(O, B, A);
                    const uint32_t got = bits(O[r * 4 + l]);
                    mul_a_first[r][k][l] = got == kP1 ? 1 : got == kP2 ? 0 : 9;
                }
                // add order (steps 1..3): out after step k-1 = P3 (B[k-1][l] = P3), product of step k = P1 (a[r][k])
                if (k >= 1) {
                    float A[16], B[16], O[16];
                    for (int i = 0; i < 16; ++i) { A[i] = 1.0f + i; B[i] = 2.0f + i; O[i] = 0; }
                    std::memcpy(&A[r * 4 + k], &kP1, 4);
                    if (variant == 1 && k - 1 == 3) continue;
                    std::memcpy(&B[(k - 1) * 4 + l], &kP3, 4);
                    if (variant == 0) reinterpret_cast<F3>(0x4144D0)(O, A, B);
                    else reinterpret_cast<F3>(0x416100)(O, B, A);
                    const uint32_t got = bits(O[r * 4 + l]);
                    add_out_first[r][k][l] = got == kP3 ? 1 : got == kP1 ? 0 : 9;
                }
            }
    std::printf("variant %d multiply a-first [row][step][lane]:\n", variant);
    for (int r = 0; r < rows; ++r) {
        for (int k = 0; k < 4; ++k) std::printf(" %u%u%u%u", mul_a_first[r][k][0], mul_a_first[r][k][1], mul_a_first[r][k][2], mul_a_first[r][k][3]);
        std::printf("\n");
    }
    std::printf("variant %d add out-first [row][step][lane] (step 0 unused):\n", variant);
    for (int r = 0; r < rows; ++r) {
        for (int k = 0; k < 4; ++k) std::printf(" %u%u%u%u", add_out_first[r][k][0], add_out_first[r][k][1], add_out_first[r][k][2], add_out_first[r][k][3]);
        std::printf("\n");
    }
}
