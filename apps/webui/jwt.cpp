#include "jwt.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <vector>
#include <cstring>

namespace dztrader::webui {

namespace {

constexpr const char* HEADER_JSON = R"({"alg":"HS256","typ":"JWT"})";

/// Base64url 编码（无 padding）
std::string base64url_encode(const unsigned char* data, size_t len) {
    // 先 base64
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    // 转 url-safe + 去 padding
    for (auto& c : result) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    while (!result.empty() && result.back() == '=') {
        result.pop_back();
    }
    return result;
}

std::string base64url_encode_str(const std::string& s) {
    return base64url_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

/// Base64url 解码（补 padding）
std::string base64url_decode(const std::string& s) {
    std::string padded = s;
    for (auto& c : padded) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        }
    }
    while (padded.size() % 4 != 0) {
        padded += '=';
    }

    std::vector<unsigned char> buf(padded.size());
    const int len =
        EVP_DecodeBlock(buf.data(), reinterpret_cast<const unsigned char*>(padded.data()),
                        static_cast<int>(padded.size()));
    if (len <= 0) {
        return {};
    }
    // EVP_DecodeBlock 补了 padding 字节，需去掉
    size_t pad_count = 0;
    if (padded.size() >= 2 && padded[padded.size() - 1] == '=') {
        pad_count++;
    }
    if (padded.size() >= 4 && padded[padded.size() - 2] == '=') {
        pad_count++;
    }
    return {reinterpret_cast<char*>(buf.data()), static_cast<size_t>(len) - pad_count};
}

/// HMAC-SHA256
std::string hmac_sha256(const std::string& key, const std::string& msg) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         digest, &digest_len);
    return {reinterpret_cast<char*>(digest), digest_len};
}

}  // namespace

std::string jwt_sign(const std::string& sub, uint32_t ttl_sec, const std::string& secret) {
    const std::string header_b64 = base64url_encode_str(HEADER_JSON);

    auto now = std::chrono::system_clock::now();
    auto exp = now + std::chrono::seconds(ttl_sec);
    const nlohmann::json payload = {
        {"sub", sub},
        {"iat",
         static_cast<int64_t>(
             std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count())},
        {"exp",
         static_cast<int64_t>(
             std::chrono::duration_cast<std::chrono::seconds>(exp.time_since_epoch()).count())}};
    const std::string payload_b64 = base64url_encode_str(payload.dump());

    const std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig = hmac_sha256(secret, signing_input);
    const std::string sig_b64 =
        base64url_encode(reinterpret_cast<const unsigned char*>(sig.data()), sig.size());

    return signing_input + "." + sig_b64;
}

bool jwt_verify(const std::string& token, const std::string& secret, std::string& out_sub) {
    const size_t dot1 = token.find('.');
    const size_t dot2 = token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) {
        return false;
    }

    const std::string header_b64 = token.substr(0, dot1);
    const std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string sig_b64 = token.substr(dot2 + 1);

    const std::string signing_input = header_b64 + "." + payload_b64;
    std::string expected_sig = hmac_sha256(secret, signing_input);
    std::string expected_sig_b64 = base64url_encode(
        reinterpret_cast<const unsigned char*>(expected_sig.data()), expected_sig.size());

    // 常量时间比较
    if (expected_sig_b64.size() != sig_b64.size()) {
        return false;
    }
    int diff = 0;
    for (size_t i = 0; i < sig_b64.size(); ++i) {
        diff |= static_cast<unsigned char>(sig_b64[i]) ^ static_cast<unsigned char>(expected_sig_b64[i]);
    }
    if (diff != 0) {
        return false;
    }

    // 解析 payload，检查 exp
    const std::string payload_json = base64url_decode(payload_b64);
    if (payload_json.empty()) {
        return false;
    }
    try {
        auto payload = nlohmann::json::parse(payload_json);
        out_sub = payload.value("sub", "");
        int64_t const exp = payload.value("exp", 0LL);
        auto now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        return now < exp;
    } catch (...) {
        return false;
    }
}

}  // namespace dztrader::webui
