#include <stdint.h>

/* SHA-256 constants */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

int main(void) {
    /* SHA-256("abc") - computed inline to avoid function call ABI issues */

    /* Prepare block: "abc" + SHA-256 padding */
    uint8_t block[64];
    for (int i = 0; i < 64; i++) block[i] = 0;
    block[0] = 'a'; block[1] = 'b'; block[2] = 'c'; block[3] = 0x80;
    block[63] = 24; /* length in bits = 3*8 = 24 */

    /* Message schedule */
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24)
             | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] << 8)
             | ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    /* Compression */
    uint32_t a = 0x6a09e667, b = 0xbb67ae85, c = 0x3c6ef372, d = 0xa54ff53a;
    uint32_t e = 0x510e527f, f = 0x9b05688c, g = 0x1f83d9ab, h = 0x5be0cd19;

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    /* Final hash */
    uint32_t hash[8];
    hash[0] = a + 0x6a09e667; hash[1] = b + 0xbb67ae85;
    hash[2] = c + 0x3c6ef372; hash[3] = d + 0xa54ff53a;
    hash[4] = e + 0x510e527f; hash[5] = f + 0x9b05688c;
    hash[6] = g + 0x1f83d9ab; hash[7] = h + 0x5be0cd19;

    /* Verify: SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223
     *                          b00361a3 96177a9c b410ff61 f20015ad */
    int pass = 0;
    if (hash[0] == 0xba7816bf) pass++;
    if (hash[1] == 0x8f01cfea) pass++;
    if (hash[2] == 0x414140de) pass++;
    if (hash[3] == 0x5dae2223) pass++;
    if (hash[4] == 0xb00361a3) pass++;
    if (hash[5] == 0x96177a9c) pass++;
    if (hash[6] == 0xb410ff61) pass++;
    if (hash[7] == 0xf20015ad) pass++;

    return 8-pass;  /* Expected: 8 */
}
