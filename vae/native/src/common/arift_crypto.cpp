#include "arift_crypto.h"

#include <cstring>

#include "arift_utils.h"

namespace arift {
namespace crypto {

// ---------------------------------------------------------------------------
// ChaCha20
// ---------------------------------------------------------------------------

namespace {
uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

uint32_t load32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void store32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

constexpr uint32_t kSigma[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
}  // namespace

void ChaCha20::init(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
    state_[0] = kSigma[0];
    state_[1] = kSigma[1];
    state_[2] = kSigma[2];
    state_[3] = kSigma[3];
    for (int i = 0; i < 8; ++i) {
        state_[4 + i] = load32le(key + i * 4);
    }
    state_[12] = counter;
    state_[13] = load32le(nonce);
    state_[14] = load32le(nonce + 4);
    state_[15] = load32le(nonce + 8);
}

void ChaCha20::nextBlock(uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, state_, sizeof(x));
    for (int round = 0; round < 10; ++round) {
        // Column rounds
        x[0] += x[4]; x[12] = rotl32(x[12] ^ x[0], 16);
        x[8] += x[12]; x[4] = rotl32(x[4] ^ x[8], 12);
        x[0] += x[4]; x[12] = rotl32(x[12] ^ x[0], 8);
        x[8] += x[12]; x[4] = rotl32(x[4] ^ x[8], 7);

        x[1] += x[5]; x[13] = rotl32(x[13] ^ x[1], 16);
        x[9] += x[13]; x[5] = rotl32(x[5] ^ x[9], 12);
        x[1] += x[5]; x[13] = rotl32(x[13] ^ x[1], 8);
        x[9] += x[13]; x[5] = rotl32(x[5] ^ x[9], 7);

        x[2] += x[6]; x[14] = rotl32(x[14] ^ x[2], 16);
        x[10] += x[14]; x[6] = rotl32(x[6] ^ x[10], 12);
        x[2] += x[6]; x[14] = rotl32(x[14] ^ x[2], 8);
        x[10] += x[14]; x[6] = rotl32(x[6] ^ x[10], 7);

        x[3] += x[7]; x[15] = rotl32(x[15] ^ x[3], 16);
        x[11] += x[15]; x[7] = rotl32(x[7] ^ x[11], 12);
        x[3] += x[7]; x[15] = rotl32(x[15] ^ x[3], 8);
        x[11] += x[15]; x[7] = rotl32(x[7] ^ x[11], 7);

        // Diagonal rounds
        x[0] += x[5]; x[15] = rotl32(x[15] ^ x[0], 16);
        x[10] += x[15]; x[5] = rotl32(x[5] ^ x[10], 12);
        x[0] += x[5]; x[15] = rotl32(x[15] ^ x[0], 8);
        x[10] += x[15]; x[5] = rotl32(x[5] ^ x[10], 7);

        x[1] += x[6]; x[12] = rotl32(x[12] ^ x[1], 16);
        x[11] += x[12]; x[6] = rotl32(x[6] ^ x[11], 12);
        x[1] += x[6]; x[12] = rotl32(x[12] ^ x[1], 8);
        x[11] += x[12]; x[6] = rotl32(x[6] ^ x[11], 7);

        x[2] += x[7]; x[13] = rotl32(x[13] ^ x[2], 16);
        x[8] += x[13]; x[7] = rotl32(x[7] ^ x[8], 12);
        x[2] += x[7]; x[13] = rotl32(x[13] ^ x[2], 8);
        x[8] += x[13]; x[7] = rotl32(x[7] ^ x[8], 7);

        x[3] += x[4]; x[14] = rotl32(x[14] ^ x[3], 16);
        x[9] += x[14]; x[4] = rotl32(x[4] ^ x[9], 12);
        x[3] += x[4]; x[14] = rotl32(x[14] ^ x[3], 8);
        x[9] += x[14]; x[4] = rotl32(x[4] ^ x[9], 7);
    }
    for (int i = 0; i < 16; ++i) {
        store32le(out + i * 4, x[i] + state_[i]);
    }
    ++state_[12];
}

void ChaCha20::crypt(const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        nextBlock(block);
        size_t n = (len - off) < 64 ? (len - off) : 64;
        for (size_t i = 0; i < n; ++i) {
            out[off + i] = in[off + i] ^ block[i];
        }
        off += n;
    }
}

std::string xorBlob(const std::string& data, const std::string& key) {
    if (key.empty()) return data;
    std::string out = data;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(out[i] ^ key[i % key.size()]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// AES-256-CBC
// ---------------------------------------------------------------------------

namespace {
constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

constexpr uint8_t kInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}
}  // namespace

bool Aes256Cbc::setKey(const uint8_t key[32]) {
    memcpy(key_, key, 32);
    memset(iv_, 0, 16);
    expandKey();
    return true;
}

void Aes256Cbc::expandKey() {
    constexpr uint8_t rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    memcpy(rk_, key_, 32);
    int words = 4;
    while (words < 60) {
        uint8_t t[4];
        memcpy(t, rk_ + (words - 1) * 4, 4);
        if (words % 8 == 0) {
            uint8_t tmp = t[0];
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
            for (int i = 0; i < 4; ++i) t[i] = kSbox[t[i]];
            t[0] ^= rcon[words / 8];
        } else if (words % 8 == 4) {
            for (int i = 0; i < 4; ++i) t[i] = kSbox[t[i]];
        }
        for (int i = 0; i < 4; ++i) {
            rk_[words * 4 + i] = rk_[(words - 8) * 4 + i] ^ t[i];
        }
        ++words;
    }
}

void Aes256Cbc::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; ++i) s[i] ^= rk_[i];
    for (int round = 1; round <= 14; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
        uint8_t t[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                t[r * 4 + c] = s[((r + c) % 4) * 4 + c];
            }
        }
        memcpy(s, t, 16);
        if (round < 14) {
            uint8_t u[16];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    u[r * 4 + c] = gmul(s[r * 4], 2) ^ gmul(s[r * 4 + 1], 3) ^
                                   s[r * 4 + 2] ^ s[r * 4 + 3];
                }
            }
            memcpy(s, u, 16);
        }
        for (int i = 0; i < 16; ++i) s[i] ^= rk_[round * 16 + i];
    }
    memcpy(out, s, 16);
}

void Aes256Cbc::decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; ++i) s[i] ^= rk_[14 * 16 + i];
    for (int round = 14; round >= 1; --round) {
        uint8_t t[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                t[((r + c) % 4) * 4 + c] = s[r * 4 + c];
            }
        }
        memcpy(s, t, 16);
        for (int i = 0; i < 16; ++i) s[i] = kInvSbox[s[i]];
        if (round > 1) {
            uint8_t u[16];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    u[r * 4 + c] = gmul(s[r * 4], 14) ^ gmul(s[r * 4 + 1], 11) ^
                                   gmul(s[r * 4 + 2], 13) ^ gmul(s[r * 4 + 3], 9);
                }
            }
            memcpy(s, u, 16);
        }
        for (int i = 0; i < 16; ++i) s[i] ^= rk_[(round - 1) * 16 + i];
    }
    memcpy(out, s, 16);
}

std::vector<uint8_t> Aes256Cbc::encrypt(const std::vector<uint8_t>& plain) const {
    size_t padded = (plain.size() / 16 + 1) * 16;
    std::vector<uint8_t> out(padded);
    uint8_t prev[16];
    memcpy(prev, iv_, 16);
    size_t i = 0;
    for (; i < plain.size(); i += 16) {
        uint8_t block[16];
        size_t n = plain.size() - i;
        if (n >= 16) {
            memcpy(block, plain.data() + i, 16);
        } else {
            memset(block, static_cast<int>(16 - n), 16);
            memcpy(block, plain.data() + i, n);
        }
        for (int k = 0; k < 16; ++k) block[k] ^= prev[k];
        encryptBlock(block, out.data() + i);
        memcpy(prev, out.data() + i, 16);
    }
    if (i < padded) {
        uint8_t block[16];
        memset(block, 16, 16);
        for (int k = 0; k < 16; ++k) block[k] ^= prev[k];
        encryptBlock(block, out.data() + i);
    }
    return out;
}

std::vector<uint8_t> Aes256Cbc::decrypt(const std::vector<uint8_t>& cipher) const {
    if (cipher.empty() || cipher.size() % 16 != 0) return {};
    std::vector<uint8_t> out(cipher.size());
    uint8_t prev[16];
    memcpy(prev, iv_, 16);
    for (size_t i = 0; i < cipher.size(); i += 16) {
        uint8_t block[16];
        decryptBlock(cipher.data() + i, block);
        for (int k = 0; k < 16; ++k) block[k] ^= prev[k];
        memcpy(out.data() + i, block, 16);
        memcpy(prev, cipher.data() + i, 16);
    }
    uint8_t pad = out.back();
    if (pad >= 1 && pad <= 16) out.resize(out.size() - pad);
    return out;
}

uint64_t hmac64(const void* data, size_t len, uint64_t secret) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = secret ^ 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
        h ^= h >> 32;
    }
    return h;
}

std::string obfDecode(const uint8_t* buf, size_t len, uint64_t seed) {
    std::string out;
    out.reserve(len);
    uint64_t x = seed;
    for (size_t i = 0; i < len; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        uint8_t key = static_cast<uint8_t>(x >> 33);
        out.push_back(static_cast<char>(buf[i] ^ key));
    }
    return out;
}

}  // namespace crypto
}  // namespace arift