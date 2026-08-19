#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arift {
namespace crypto {

// Symmetric stream cipher (ChaCha20-style) for payload obfuscation.
class ChaCha20 {
public:
    void init(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter = 0);
    void crypt(const uint8_t* in, uint8_t* out, size_t len);

private:
    uint32_t state_[16];
    void nextBlock(uint8_t out[64]);
};

std::string xorBlob(const std::string& data, const std::string& key);

// AES-256-CBC without external deps (pure implementation).
class Aes256Cbc {
public:
    bool setKey(const uint8_t key[32]);
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plain) const;
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& cipher) const;

private:
    uint8_t key_[32];
    uint8_t rk_[240];
    uint8_t iv_[16];
    void expandKey();
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;
};

uint64_t hmac64(const void* data, size_t len, uint64_t secret);

// Obfuscated string helpers (decrypt at runtime).
std::string obfDecode(const uint8_t* buf, size_t len, uint64_t seed);

}  // namespace crypto
}  // namespace arift