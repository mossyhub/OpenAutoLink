#include "openautolink/carplay_crypto.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <sys/stat.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace openautolink {

// ── SRP-6a constants (3072-bit group per RFC 5054) ───────────────

// N = 3072-bit safe prime from RFC 5054, Appendix A
static const char* kSrpN_hex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C"
    "7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A08"
    "64D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946"
    "E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

static const char* kSrpG_hex = "05";  // Generator g = 5

// ── Constructor / Destructor ─────────────────────────────────────

CarPlayCrypto::CarPlayCrypto()
{
    // Zero-initialize all key arrays
    ltsk_.fill(0);
    ltpk_.fill(0);
    peer_ltpk_.fill(0);
    read_key_.fill(0);
    write_key_.fill(0);
    verify_secret_.fill(0);
    verify_our_pubkey_.fill(0);
    verify_our_privkey_.fill(0);
    verify_peer_pubkey_.fill(0);
}

CarPlayCrypto::~CarPlayCrypto() = default;

// ── Identity Management ──────────────────────────────────────────

bool CarPlayCrypto::init_identity(const std::string& keys_path)
{
    keys_path_ = keys_path;

    // Try to load existing keys
    if (load_keys()) {
        identity_initialized_ = true;
        std::cerr << "[CarPlayCrypto] loaded identity from " << keys_path << std::endl;
        return true;
    }

    // Generate new Ed25519 keypair
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        std::cerr << "[CarPlayCrypto] EVP_PKEY_CTX_new_id failed" << std::endl;
        return false;
    }

    bool ok = false;
    if (EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0) {
        size_t len = 32;
        if (EVP_PKEY_get_raw_private_key(pkey, ltsk_.data(), &len) > 0 &&
            EVP_PKEY_get_raw_public_key(pkey, ltpk_.data(), &len) > 0) {
            ok = true;
        }
    }

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);

    if (!ok) {
        std::cerr << "[CarPlayCrypto] Ed25519 keygen failed" << std::endl;
        return false;
    }

    identity_initialized_ = true;
    save_keys();
    std::cerr << "[CarPlayCrypto] generated new identity" << std::endl;
    return true;
}

bool CarPlayCrypto::save_keys()
{
    if (keys_path_.empty()) return false;

    // Create directory if needed
    auto last_slash = keys_path_.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir = keys_path_.substr(0, last_slash);
        mkdir(dir.c_str(), 0700);
    }

    std::ofstream f(keys_path_, std::ios::binary);
    if (!f) {
        std::cerr << "[CarPlayCrypto] failed to save keys to " << keys_path_
                  << ": " << strerror(errno) << std::endl;
        return false;
    }

    // Simple binary format: magic(4) + ltsk(32) + ltpk(32) + has_peer(1) + [peer_ltpk(32) + id_len(2) + id]
    const char magic[] = "OALk";
    f.write(magic, 4);
    f.write(reinterpret_cast<const char*>(ltsk_.data()), 32);
    f.write(reinterpret_cast<const char*>(ltpk_.data()), 32);

    uint8_t has_peer = has_peer_ltpk_ ? 1 : 0;
    f.write(reinterpret_cast<const char*>(&has_peer), 1);

    if (has_peer_ltpk_) {
        f.write(reinterpret_cast<const char*>(peer_ltpk_.data()), 32);
        uint16_t id_len = static_cast<uint16_t>(peer_identifier_.size());
        f.write(reinterpret_cast<const char*>(&id_len), 2);
        f.write(peer_identifier_.data(), id_len);
    }

    // Set restrictive permissions (keys are sensitive)
    chmod(keys_path_.c_str(), 0600);
    return true;
}

bool CarPlayCrypto::load_keys()
{
    std::ifstream f(keys_path_, std::ios::binary);
    if (!f) return false;

    char magic[4]{};
    f.read(magic, 4);
    if (memcmp(magic, "OALk", 4) != 0) return false;

    f.read(reinterpret_cast<char*>(ltsk_.data()), 32);
    f.read(reinterpret_cast<char*>(ltpk_.data()), 32);

    uint8_t has_peer = 0;
    f.read(reinterpret_cast<char*>(&has_peer), 1);

    if (has_peer) {
        f.read(reinterpret_cast<char*>(peer_ltpk_.data()), 32);
        uint16_t id_len = 0;
        f.read(reinterpret_cast<char*>(&id_len), 2);
        if (id_len > 0 && id_len < 256) {
            peer_identifier_.resize(id_len);
            f.read(peer_identifier_.data(), id_len);
            has_peer_ltpk_ = true;
        }
    }

    return f.good();
}

// ── TLV8 Encoding/Decoding ──────────────────────────────────────

std::vector<CarPlayCrypto::Tlv8> CarPlayCrypto::parse_tlv8(const uint8_t* data, size_t len)
{
    std::vector<Tlv8> items;
    size_t offset = 0;

    while (offset + 2 <= len) {
        uint8_t type = data[offset];
        uint8_t length = data[offset + 1];
        offset += 2;

        if (offset + length > len) break;

        // Check if this is a continuation of the previous TLV with the same type
        if (!items.empty() && items.back().type == type && length == 255) {
            // Previous fragment was max length — this is a continuation
            items.back().value.insert(items.back().value.end(),
                                       data + offset, data + offset + length);
        } else if (!items.empty() && items.back().type == type &&
                   items.back().value.size() % 255 == 0) {
            // Continuation fragment
            items.back().value.insert(items.back().value.end(),
                                       data + offset, data + offset + length);
        } else {
            Tlv8 item;
            item.type = type;
            item.value.assign(data + offset, data + offset + length);
            items.push_back(std::move(item));
        }

        offset += length;
    }

    return items;
}

std::vector<uint8_t> CarPlayCrypto::build_tlv8(const std::vector<Tlv8>& items)
{
    std::vector<uint8_t> result;

    for (const auto& item : items) {
        const uint8_t* ptr = item.value.data();
        size_t remaining = item.value.size();

        // TLV8 fragments: max 255 bytes per fragment
        do {
            uint8_t frag_len = static_cast<uint8_t>(std::min(remaining, size_t{255}));
            result.push_back(item.type);
            result.push_back(frag_len);
            result.insert(result.end(), ptr, ptr + frag_len);
            ptr += frag_len;
            remaining -= frag_len;
        } while (remaining > 0);

        // Handle zero-length TLV
        if (item.value.empty()) {
            result.push_back(item.type);
            result.push_back(0);
        }
    }

    return result;
}

const std::vector<uint8_t>* CarPlayCrypto::find_tlv(const std::vector<Tlv8>& items, uint8_t type)
{
    for (const auto& item : items) {
        if (item.type == type) return &item.value;
    }
    return nullptr;
}

// ── Pair-Setup (SRP-6a) ──────────────────────────────────────────

std::vector<uint8_t> CarPlayCrypto::handle_pair_setup(const std::vector<uint8_t>& request)
{
    auto tlvs = parse_tlv8(request.data(), request.size());

    const auto* state_tlv = find_tlv(tlvs, kTlvType_State);
    if (!state_tlv || state_tlv->empty()) {
        std::cerr << "[CarPlayCrypto] pair-setup: missing state TLV" << std::endl;
        return build_tlv8({{kTlvType_State, {0x02}}, {kTlvType_Error, {0x02}}});
    }

    uint8_t state = (*state_tlv)[0];

    switch (state) {
    case 0x01: return handle_pair_setup_m1(tlvs);
    case 0x03: return handle_pair_setup_m3(tlvs);
    case 0x05: return handle_pair_setup_m5(tlvs);
    default:
        std::cerr << "[CarPlayCrypto] pair-setup: unexpected state " << (int)state << std::endl;
        return build_tlv8({{kTlvType_State, {static_cast<uint8_t>(state + 1)}},
                           {kTlvType_Error, {0x02}}});
    }
}

std::vector<uint8_t> CarPlayCrypto::handle_pair_setup_m1(
    [[maybe_unused]] const std::vector<Tlv8>& request_tlvs)
{
    // M1: iPhone requests pairing. We generate SRP parameters and a PIN.
    std::cerr << "[CarPlayCrypto] pair-setup M1 received — generating SRP params" << std::endl;

    // Generate 4-digit PIN
    setup_pin_ = generate_pin();
    std::cerr << "[CarPlayCrypto] pairing PIN: " << setup_pin_ << std::endl;

    // Notify car app of PIN for display
    if (pin_cb_) {
        pin_cb_(setup_pin_);
    }

    // Generate SRP salt (16 bytes random)
    srp_salt_.resize(16);
    RAND_bytes(srp_salt_.data(), 16);

    // SRP-6a: compute verifier v and server public key B
    //
    // x = SHA-512(salt || SHA-512("Pair-Setup" || ":" || PIN))
    // v = g^x mod N
    // b = random(32 bytes)
    // B = (k*v + g^b) mod N
    // k = SHA-512(N || pad(g))

    BIGNUM* N = BN_new();
    BIGNUM* g = BN_new();
    BN_hex2bn(&N, kSrpN_hex);
    BN_hex2bn(&g, kSrpG_hex);
    BN_CTX* bn_ctx = BN_CTX_new();

    // x = H(salt || H("Pair-Setup:PIN"))
    // Inner hash: SHA-512("Pair-Setup:PIN")
    std::string identity = "Pair-Setup:" + setup_pin_;
    uint8_t inner_hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const uint8_t*>(identity.data()), identity.size(), inner_hash);

    // Outer hash: SHA-512(salt || inner_hash)
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, srp_salt_.data(), srp_salt_.size());
    EVP_DigestUpdate(md_ctx, inner_hash, SHA512_DIGEST_LENGTH);
    uint8_t x_hash[SHA512_DIGEST_LENGTH];
    unsigned int x_hash_len = 0;
    EVP_DigestFinal_ex(md_ctx, x_hash, &x_hash_len);
    EVP_MD_CTX_free(md_ctx);

    BIGNUM* x = BN_bin2bn(x_hash, SHA512_DIGEST_LENGTH, nullptr);

    // v = g^x mod N
    BIGNUM* v = BN_new();
    BN_mod_exp(v, g, x, N, bn_ctx);

    // Store verifier
    srp_v_.resize(BN_num_bytes(v));
    BN_bn2bin(v, srp_v_.data());

    // b = random 32 bytes (server private)
    srp_b_.resize(32);
    RAND_bytes(srp_b_.data(), 32);
    BIGNUM* b = BN_bin2bn(srp_b_.data(), 32, nullptr);

    // k = SHA-512(N || pad(g))
    size_t N_len = BN_num_bytes(N);
    std::vector<uint8_t> N_bytes(N_len);
    BN_bn2bin(N, N_bytes.data());
    std::vector<uint8_t> g_padded(N_len, 0);
    size_t g_len = BN_num_bytes(g);
    BN_bn2bin(g, g_padded.data() + (N_len - g_len));

    md_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, N_bytes.data(), N_len);
    EVP_DigestUpdate(md_ctx, g_padded.data(), N_len);
    uint8_t k_hash[SHA512_DIGEST_LENGTH];
    unsigned int k_hash_len = 0;
    EVP_DigestFinal_ex(md_ctx, k_hash, &k_hash_len);
    EVP_MD_CTX_free(md_ctx);

    BIGNUM* k = BN_bin2bn(k_hash, SHA512_DIGEST_LENGTH, nullptr);

    // B = (k*v + g^b) mod N
    BIGNUM* kv = BN_new();
    BN_mod_mul(kv, k, v, N, bn_ctx);
    BIGNUM* gb = BN_new();
    BN_mod_exp(gb, g, b, N, bn_ctx);
    BIGNUM* B = BN_new();
    BN_mod_add(B, kv, gb, N, bn_ctx);

    srp_B_.resize(BN_num_bytes(B));
    BN_bn2bin(B, srp_B_.data());

    // Pad B to N_len
    if (srp_B_.size() < N_len) {
        srp_B_.insert(srp_B_.begin(), N_len - srp_B_.size(), 0);
    }

    BN_free(N); BN_free(g); BN_free(x); BN_free(v); BN_free(b);
    BN_free(k); BN_free(kv); BN_free(gb); BN_free(B);
    BN_CTX_free(bn_ctx);

    pair_setup_state_ = PairSetupState::WaitingM3;

    // M2 response: state=2, salt, public_key(B)
    return build_tlv8({
        {kTlvType_State, {0x02}},
        {kTlvType_Salt, srp_salt_},
        {kTlvType_PublicKey, srp_B_}
    });
}

std::vector<uint8_t> CarPlayCrypto::handle_pair_setup_m3(const std::vector<Tlv8>& request_tlvs)
{
    // M3: iPhone sends SRP public key A and proof M1
    std::cerr << "[CarPlayCrypto] pair-setup M3 received — verifying proof" << std::endl;

    if (pair_setup_state_ != PairSetupState::WaitingM3) {
        std::cerr << "[CarPlayCrypto] pair-setup M3: unexpected state" << std::endl;
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    const auto* pk_tlv = find_tlv(request_tlvs, kTlvType_PublicKey);
    const auto* proof_tlv = find_tlv(request_tlvs, kTlvType_Proof);

    if (!pk_tlv || !proof_tlv) {
        std::cerr << "[CarPlayCrypto] pair-setup M3: missing A or proof" << std::endl;
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    const auto& A_bytes = *pk_tlv;
    const auto& M1_proof = *proof_tlv;

    BIGNUM* N = BN_new();
    BIGNUM* g = BN_new();
    BN_hex2bn(&N, kSrpN_hex);
    BN_hex2bn(&g, kSrpG_hex);
    BN_CTX* bn_ctx = BN_CTX_new();

    BIGNUM* A = BN_bin2bn(A_bytes.data(), A_bytes.size(), nullptr);
    BIGNUM* B = BN_bin2bn(srp_B_.data(), srp_B_.size(), nullptr);

    // Check A mod N != 0 (SRP-6a safety check)
    BIGNUM* A_mod = BN_new();
    BN_mod(A_mod, A, N, bn_ctx);
    if (BN_is_zero(A_mod)) {
        std::cerr << "[CarPlayCrypto] pair-setup M3: A mod N = 0 (attack)" << std::endl;
        BN_free(N); BN_free(g); BN_free(A); BN_free(B); BN_free(A_mod); BN_CTX_free(bn_ctx);
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }
    BN_free(A_mod);

    size_t N_len = BN_num_bytes(N);

    // u = SHA-512(pad(A) || pad(B))
    std::vector<uint8_t> A_padded(N_len, 0);
    size_t A_len = BN_num_bytes(A);
    BN_bn2bin(A, A_padded.data() + (N_len - A_len));

    std::vector<uint8_t> B_padded(N_len, 0);
    size_t B_len = BN_num_bytes(B);
    BN_bn2bin(B, B_padded.data() + (N_len - B_len));

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, A_padded.data(), N_len);
    EVP_DigestUpdate(md_ctx, B_padded.data(), N_len);
    uint8_t u_hash[SHA512_DIGEST_LENGTH];
    unsigned int u_hash_len = 0;
    EVP_DigestFinal_ex(md_ctx, u_hash, &u_hash_len);
    EVP_MD_CTX_free(md_ctx);

    BIGNUM* u = BN_bin2bn(u_hash, SHA512_DIGEST_LENGTH, nullptr);

    // S = (A * v^u) ^ b mod N
    BIGNUM* v = BN_bin2bn(srp_v_.data(), srp_v_.size(), nullptr);
    BIGNUM* b = BN_bin2bn(srp_b_.data(), srp_b_.size(), nullptr);

    BIGNUM* vu = BN_new();
    BN_mod_exp(vu, v, u, N, bn_ctx);

    BIGNUM* Avu = BN_new();
    BN_mod_mul(Avu, A, vu, N, bn_ctx);

    BIGNUM* S = BN_new();
    BN_mod_exp(S, Avu, b, N, bn_ctx);

    // K = SHA-512(S) — session key
    std::vector<uint8_t> S_bytes(BN_num_bytes(S));
    BN_bn2bin(S, S_bytes.data());

    uint8_t K[SHA512_DIGEST_LENGTH];
    SHA512(S_bytes.data(), S_bytes.size(), K);
    srp_session_key_.assign(K, K + SHA512_DIGEST_LENGTH);

    // Compute expected M1:
    // M1 = SHA-512(SHA-512(N) XOR SHA-512(g) || SHA-512("Pair-Setup") || salt || A || B || K)

    // H(N) XOR H(g)
    uint8_t h_N[SHA512_DIGEST_LENGTH], h_g[SHA512_DIGEST_LENGTH];
    std::vector<uint8_t> N_bytes(N_len);
    BN_bn2bin(N, N_bytes.data());
    SHA512(N_bytes.data(), N_len, h_N);

    std::vector<uint8_t> g_bytes(BN_num_bytes(g));
    BN_bn2bin(g, g_bytes.data());
    SHA512(g_bytes.data(), g_bytes.size(), h_g);

    uint8_t h_Ng_xor[SHA512_DIGEST_LENGTH];
    for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i) {
        h_Ng_xor[i] = h_N[i] ^ h_g[i];
    }

    // H("Pair-Setup")
    const std::string identity = "Pair-Setup";
    uint8_t h_identity[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const uint8_t*>(identity.data()), identity.size(), h_identity);

    // M1_expected = H(H(N) XOR H(g) || H(I) || salt || A || B || K)
    md_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, h_Ng_xor, SHA512_DIGEST_LENGTH);
    EVP_DigestUpdate(md_ctx, h_identity, SHA512_DIGEST_LENGTH);
    EVP_DigestUpdate(md_ctx, srp_salt_.data(), srp_salt_.size());
    EVP_DigestUpdate(md_ctx, A_padded.data(), N_len);
    EVP_DigestUpdate(md_ctx, B_padded.data(), N_len);
    EVP_DigestUpdate(md_ctx, K, SHA512_DIGEST_LENGTH);
    uint8_t M1_expected[SHA512_DIGEST_LENGTH];
    unsigned int M1_len = 0;
    EVP_DigestFinal_ex(md_ctx, M1_expected, &M1_len);
    EVP_MD_CTX_free(md_ctx);

    // Verify M1
    if (M1_proof.size() != SHA512_DIGEST_LENGTH ||
        CRYPTO_memcmp(M1_proof.data(), M1_expected, SHA512_DIGEST_LENGTH) != 0) {
        std::cerr << "[CarPlayCrypto] pair-setup M3: proof verification FAILED (wrong PIN?)" << std::endl;
        pair_setup_state_ = PairSetupState::Idle;
        BN_free(N); BN_free(g); BN_free(A); BN_free(B);
        BN_free(u); BN_free(v); BN_free(b);
        BN_free(vu); BN_free(Avu); BN_free(S);
        BN_CTX_free(bn_ctx);
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    std::cerr << "[CarPlayCrypto] pair-setup M3: proof verified OK" << std::endl;

    // Compute M2 (server proof for iPhone):
    // M2 = SHA-512(A || M1 || K)
    md_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, A_padded.data(), N_len);
    EVP_DigestUpdate(md_ctx, M1_expected, SHA512_DIGEST_LENGTH);
    EVP_DigestUpdate(md_ctx, K, SHA512_DIGEST_LENGTH);
    uint8_t M2_proof[SHA512_DIGEST_LENGTH];
    unsigned int M2_len = 0;
    EVP_DigestFinal_ex(md_ctx, M2_proof, &M2_len);
    EVP_MD_CTX_free(md_ctx);

    BN_free(N); BN_free(g); BN_free(A); BN_free(B);
    BN_free(u); BN_free(v); BN_free(b);
    BN_free(vu); BN_free(Avu); BN_free(S);
    BN_CTX_free(bn_ctx);

    pair_setup_state_ = PairSetupState::WaitingM5;

    // M4 response: state=4, proof(M2)
    std::vector<uint8_t> proof_vec(M2_proof, M2_proof + SHA512_DIGEST_LENGTH);
    return build_tlv8({
        {kTlvType_State, {0x04}},
        {kTlvType_Proof, proof_vec}
    });
}

std::vector<uint8_t> CarPlayCrypto::handle_pair_setup_m5(const std::vector<Tlv8>& request_tlvs)
{
    // M5: iPhone sends encrypted LTPK exchange
    std::cerr << "[CarPlayCrypto] pair-setup M5 received — exchanging long-term keys" << std::endl;

    if (pair_setup_state_ != PairSetupState::WaitingM5) {
        return build_tlv8({{kTlvType_State, {0x06}}, {kTlvType_Error, {0x02}}});
    }

    const auto* enc_tlv = find_tlv(request_tlvs, kTlvType_EncryptedData);
    if (!enc_tlv || enc_tlv->size() < 16) {
        return build_tlv8({{kTlvType_State, {0x06}}, {kTlvType_Error, {0x02}}});
    }

    // Derive encryption key for M5/M6 from SRP session key
    auto session_key = hkdf_sha512(
        srp_session_key_.data(), srp_session_key_.size(),
        "Pair-Setup-Encrypt-Salt",
        "Pair-Setup-Encrypt-Info",
        32);

    // Decrypt M5 payload
    // Format: ciphertext(N) + auth_tag(16)
    size_t ciphertext_len = enc_tlv->size() - 16;
    const uint8_t* ciphertext = enc_tlv->data();
    const uint8_t* tag = enc_tlv->data() + ciphertext_len;

    std::vector<uint8_t> plaintext(ciphertext_len);
    // Nonce for M5: "PS-Msg05" padded to 12 bytes
    uint8_t nonce[12] = {};
    memcpy(nonce + 4, "PS-Msg05", 8);

    if (!chacha20_poly1305_decrypt(session_key.data(),
                                    nonce, 12,
                                    nullptr, 0,
                                    ciphertext, ciphertext_len,
                                    tag, plaintext.data())) {
        std::cerr << "[CarPlayCrypto] pair-setup M5: decryption failed" << std::endl;
        pair_setup_state_ = PairSetupState::Idle;
        return build_tlv8({{kTlvType_State, {0x06}}, {kTlvType_Error, {0x02}}});
    }

    // Parse inner TLV: identifier + LTPK + signature
    auto inner_tlvs = parse_tlv8(plaintext.data(), plaintext.size());
    const auto* id_tlv = find_tlv(inner_tlvs, kTlvType_Identifier);
    const auto* pk_tlv = find_tlv(inner_tlvs, kTlvType_PublicKey);
    const auto* sig_tlv = find_tlv(inner_tlvs, kTlvType_Signature);

    if (!id_tlv || !pk_tlv || !sig_tlv || pk_tlv->size() != 32 || sig_tlv->size() != 64) {
        std::cerr << "[CarPlayCrypto] pair-setup M5: invalid inner TLV" << std::endl;
        pair_setup_state_ = PairSetupState::Idle;
        return build_tlv8({{kTlvType_State, {0x06}}, {kTlvType_Error, {0x02}}});
    }

    // Verify the iPhone's signature
    // iOSDeviceX = HKDF(session_key, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", 32)
    auto device_x = hkdf_sha512(
        srp_session_key_.data(), srp_session_key_.size(),
        "Pair-Setup-Controller-Sign-Salt",
        "Pair-Setup-Controller-Sign-Info",
        32);

    // Construct the message that was signed: deviceX || identifier || LTPK
    std::vector<uint8_t> signed_msg;
    signed_msg.insert(signed_msg.end(), device_x.begin(), device_x.end());
    signed_msg.insert(signed_msg.end(), id_tlv->begin(), id_tlv->end());
    signed_msg.insert(signed_msg.end(), pk_tlv->begin(), pk_tlv->end());

    if (!ed25519_verify(pk_tlv->data(), signed_msg.data(), signed_msg.size(), sig_tlv->data())) {
        std::cerr << "[CarPlayCrypto] pair-setup M5: signature verification failed" << std::endl;
        pair_setup_state_ = PairSetupState::Idle;
        return build_tlv8({{kTlvType_State, {0x06}}, {kTlvType_Error, {0x02}}});
    }

    // Store iPhone's LTPK for future pair-verify
    std::copy(pk_tlv->begin(), pk_tlv->end(), peer_ltpk_.begin());
    peer_identifier_.assign(id_tlv->begin(), id_tlv->end());
    has_peer_ltpk_ = true;
    save_keys();

    std::cerr << "[CarPlayCrypto] pair-setup M5: stored iPhone LTPK, identifier="
              << peer_identifier_ << std::endl;

    // Build M6 response: our LTPK + signature, encrypted
    // accessoryX = HKDF(session_key, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", 32)
    auto accessory_x = hkdf_sha512(
        srp_session_key_.data(), srp_session_key_.size(),
        "Pair-Setup-Accessory-Sign-Salt",
        "Pair-Setup-Accessory-Sign-Info",
        32);

    std::string our_id = "OpenAutoLink";
    std::vector<uint8_t> our_signed_msg;
    our_signed_msg.insert(our_signed_msg.end(), accessory_x.begin(), accessory_x.end());
    our_signed_msg.insert(our_signed_msg.end(), our_id.begin(), our_id.end());
    our_signed_msg.insert(our_signed_msg.end(), ltpk_.begin(), ltpk_.end());

    uint8_t our_sig[64];
    ed25519_sign(ltsk_.data(), our_signed_msg.data(), our_signed_msg.size(), our_sig);

    // Build inner TLV
    std::vector<uint8_t> id_vec(our_id.begin(), our_id.end());
    std::vector<uint8_t> pk_vec(ltpk_.begin(), ltpk_.end());
    std::vector<uint8_t> sig_vec(our_sig, our_sig + 64);

    auto inner = build_tlv8({
        {kTlvType_Identifier, id_vec},
        {kTlvType_PublicKey, pk_vec},
        {kTlvType_Signature, sig_vec}
    });

    // Encrypt inner TLV
    std::vector<uint8_t> enc_inner(inner.size());
    uint8_t enc_tag[16];
    uint8_t m6_nonce[12] = {};
    memcpy(m6_nonce + 4, "PS-Msg06", 8);

    chacha20_poly1305_encrypt(session_key.data(),
                               m6_nonce, 12,
                               nullptr, 0,
                               inner.data(), inner.size(),
                               enc_inner.data(), enc_tag);

    std::vector<uint8_t> enc_data;
    enc_data.insert(enc_data.end(), enc_inner.begin(), enc_inner.end());
    enc_data.insert(enc_data.end(), enc_tag, enc_tag + 16);

    pair_setup_state_ = PairSetupState::Complete;

    if (pair_complete_cb_) pair_complete_cb_(true);

    std::cerr << "[CarPlayCrypto] pair-setup complete" << std::endl;

    return build_tlv8({
        {kTlvType_State, {0x06}},
        {kTlvType_EncryptedData, enc_data}
    });
}

// ── Pair-Verify (X25519) ────────────────────────────────────────

std::vector<uint8_t> CarPlayCrypto::handle_pair_verify(const std::vector<uint8_t>& request)
{
    auto tlvs = parse_tlv8(request.data(), request.size());

    const auto* state_tlv = find_tlv(tlvs, kTlvType_State);
    if (!state_tlv || state_tlv->empty()) {
        return build_tlv8({{kTlvType_State, {0x02}}, {kTlvType_Error, {0x02}}});
    }

    uint8_t state = (*state_tlv)[0];

    switch (state) {
    case 0x01: return handle_pair_verify_m1(tlvs);
    case 0x03: return handle_pair_verify_m3(tlvs);
    default:
        return build_tlv8({{kTlvType_State, {static_cast<uint8_t>(state + 1)}},
                           {kTlvType_Error, {0x02}}});
    }
}

std::vector<uint8_t> CarPlayCrypto::handle_pair_verify_m1(const std::vector<Tlv8>& request_tlvs)
{
    std::cerr << "[CarPlayCrypto] pair-verify M1 received" << std::endl;

    if (!has_peer_ltpk_) {
        std::cerr << "[CarPlayCrypto] pair-verify M1: no stored LTPK — need pair-setup first" << std::endl;
        return build_tlv8({{kTlvType_State, {0x02}}, {kTlvType_Error, {0x02}}});
    }

    const auto* pk_tlv = find_tlv(request_tlvs, kTlvType_PublicKey);
    if (!pk_tlv || pk_tlv->size() != 32) {
        return build_tlv8({{kTlvType_State, {0x02}}, {kTlvType_Error, {0x02}}});
    }

    // Store iPhone's ephemeral public key
    std::copy(pk_tlv->begin(), pk_tlv->end(), verify_peer_pubkey_.begin());

    // Generate our ephemeral X25519 keypair
    x25519_keygen(verify_our_pubkey_.data(), verify_our_privkey_.data());

    // Compute shared secret
    x25519_shared_secret(verify_our_privkey_.data(), verify_peer_pubkey_.data(), verify_secret_.data());

    // Derive session key
    auto session_key = hkdf_sha512(
        verify_secret_.data(), 32,
        "Pair-Verify-Encrypt-Salt",
        "Pair-Verify-Encrypt-Info",
        32);
    verify_session_key_ = session_key;

    // Sign our proof: sign(accessory_ltsk, accessory_ephemeral_pk || accessory_identifier || iphone_ephemeral_pk)
    std::string our_id = "OpenAutoLink";
    std::vector<uint8_t> sign_msg;
    sign_msg.insert(sign_msg.end(), verify_our_pubkey_.begin(), verify_our_pubkey_.end());
    sign_msg.insert(sign_msg.end(), our_id.begin(), our_id.end());
    sign_msg.insert(sign_msg.end(), verify_peer_pubkey_.begin(), verify_peer_pubkey_.end());

    uint8_t signature[64];
    ed25519_sign(ltsk_.data(), sign_msg.data(), sign_msg.size(), signature);

    // Build inner TLV: identifier + signature
    std::vector<uint8_t> id_vec(our_id.begin(), our_id.end());
    std::vector<uint8_t> sig_vec(signature, signature + 64);
    auto inner = build_tlv8({
        {kTlvType_Identifier, id_vec},
        {kTlvType_Signature, sig_vec}
    });

    // Encrypt inner TLV
    std::vector<uint8_t> enc_inner(inner.size());
    uint8_t enc_tag[16];
    uint8_t nonce[12] = {};
    memcpy(nonce + 4, "PV-Msg02", 8);

    chacha20_poly1305_encrypt(session_key.data(),
                               nonce, 12,
                               nullptr, 0,
                               inner.data(), inner.size(),
                               enc_inner.data(), enc_tag);

    std::vector<uint8_t> enc_data;
    enc_data.insert(enc_data.end(), enc_inner.begin(), enc_inner.end());
    enc_data.insert(enc_data.end(), enc_tag, enc_tag + 16);

    pair_verify_state_ = PairVerifyState::WaitingM3;

    // M2 response: state=2, public_key(our ephemeral), encrypted_data
    std::vector<uint8_t> our_pk_vec(verify_our_pubkey_.begin(), verify_our_pubkey_.end());
    return build_tlv8({
        {kTlvType_State, {0x02}},
        {kTlvType_PublicKey, our_pk_vec},
        {kTlvType_EncryptedData, enc_data}
    });
}

std::vector<uint8_t> CarPlayCrypto::handle_pair_verify_m3(const std::vector<Tlv8>& request_tlvs)
{
    std::cerr << "[CarPlayCrypto] pair-verify M3 received" << std::endl;

    if (pair_verify_state_ != PairVerifyState::WaitingM3) {
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    const auto* enc_tlv = find_tlv(request_tlvs, kTlvType_EncryptedData);
    if (!enc_tlv || enc_tlv->size() < 16) {
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    // Decrypt
    size_t ciphertext_len = enc_tlv->size() - 16;
    std::vector<uint8_t> plaintext(ciphertext_len);
    uint8_t nonce[12] = {};
    memcpy(nonce + 4, "PV-Msg03", 8);

    if (!chacha20_poly1305_decrypt(verify_session_key_.data(),
                                    nonce, 12,
                                    nullptr, 0,
                                    enc_tlv->data(), ciphertext_len,
                                    enc_tlv->data() + ciphertext_len,
                                    plaintext.data())) {
        std::cerr << "[CarPlayCrypto] pair-verify M3: decryption failed" << std::endl;
        pair_verify_state_ = PairVerifyState::Idle;
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    // Parse inner TLV: identifier + signature
    auto inner_tlvs = parse_tlv8(plaintext.data(), plaintext.size());
    const auto* id_tlv = find_tlv(inner_tlvs, kTlvType_Identifier);
    const auto* sig_tlv = find_tlv(inner_tlvs, kTlvType_Signature);

    if (!id_tlv || !sig_tlv || sig_tlv->size() != 64) {
        pair_verify_state_ = PairVerifyState::Idle;
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    // Verify: message = iphone_ephemeral_pk || iphone_identifier || accessory_ephemeral_pk
    std::vector<uint8_t> verify_msg;
    verify_msg.insert(verify_msg.end(), verify_peer_pubkey_.begin(), verify_peer_pubkey_.end());
    verify_msg.insert(verify_msg.end(), id_tlv->begin(), id_tlv->end());
    verify_msg.insert(verify_msg.end(), verify_our_pubkey_.begin(), verify_our_pubkey_.end());

    if (!ed25519_verify(peer_ltpk_.data(), verify_msg.data(), verify_msg.size(), sig_tlv->data())) {
        std::cerr << "[CarPlayCrypto] pair-verify M3: signature verification failed" << std::endl;
        pair_verify_state_ = PairVerifyState::Idle;
        return build_tlv8({{kTlvType_State, {0x04}}, {kTlvType_Error, {0x02}}});
    }

    // Derive transport encryption keys
    auto control_read = hkdf_sha512(
        verify_secret_.data(), 32,
        "Control-Salt", "Control-Read-Encryption-Key", 32);
    auto control_write = hkdf_sha512(
        verify_secret_.data(), 32,
        "Control-Salt", "Control-Write-Encryption-Key", 32);

    std::copy(control_read.begin(), control_read.end(), read_key_.begin());
    std::copy(control_write.begin(), control_write.end(), write_key_.begin());
    read_nonce_ = 0;
    write_nonce_ = 0;
    encryption_active_ = true;

    pair_verify_state_ = PairVerifyState::Complete;

    if (verify_complete_cb_) verify_complete_cb_(true);

    std::cerr << "[CarPlayCrypto] pair-verify complete — encrypted channel established" << std::endl;

    // M4 response: state=4 (no error = success)
    return build_tlv8({
        {kTlvType_State, {0x04}}
    });
}

// ── Transport Encryption ─────────────────────────────────────────

std::vector<uint8_t> CarPlayCrypto::encrypt(const uint8_t* data, size_t len)
{
    if (!encryption_active_) return {};

    // Build nonce: 4 bytes zero + 8 bytes little-endian counter
    uint8_t nonce[12] = {};
    memcpy(nonce + 4, &write_nonce_, 8);
    ++write_nonce_;

    // AAD: 2-byte little-endian length
    uint8_t aad[2];
    aad[0] = static_cast<uint8_t>(len & 0xFF);
    aad[1] = static_cast<uint8_t>((len >> 8) & 0xFF);

    std::vector<uint8_t> result(2 + len + 16); // length + ciphertext + tag
    result[0] = aad[0];
    result[1] = aad[1];

    chacha20_poly1305_encrypt(write_key_.data(),
                               nonce, 12,
                               aad, 2,
                               data, len,
                               result.data() + 2,
                               result.data() + 2 + len);
    return result;
}

std::vector<uint8_t> CarPlayCrypto::decrypt(const uint8_t* data, size_t len)
{
    if (!encryption_active_ || len < 18) return {};

    // First 2 bytes: plaintext length (AAD)
    uint16_t pt_len = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    if (len < static_cast<size_t>(2 + pt_len + 16)) return {};

    uint8_t nonce[12] = {};
    memcpy(nonce + 4, &read_nonce_, 8);
    ++read_nonce_;

    uint8_t aad[2] = {data[0], data[1]};

    std::vector<uint8_t> plaintext(pt_len);
    if (!chacha20_poly1305_decrypt(read_key_.data(),
                                    nonce, 12,
                                    aad, 2,
                                    data + 2, pt_len,
                                    data + 2 + pt_len,
                                    plaintext.data())) {
        std::cerr << "[CarPlayCrypto] transport decrypt failed (nonce=" << read_nonce_ - 1 << ")" << std::endl;
        return {};
    }

    return plaintext;
}

// ── Crypto Helpers ──────────────────────────────────────────────

std::vector<uint8_t> CarPlayCrypto::hkdf_sha512(
    const uint8_t* ikm, size_t ikm_len,
    const std::string& salt_str,
    const std::string& info_str,
    size_t out_len)
{
    std::vector<uint8_t> out(out_len);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return {};

    EVP_PKEY_derive_init(ctx);
    EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512());
    EVP_PKEY_CTX_set1_hkdf_salt(ctx,
        reinterpret_cast<const uint8_t*>(salt_str.data()), salt_str.size());
    EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, ikm_len);
    EVP_PKEY_CTX_add1_hkdf_info(ctx,
        reinterpret_cast<const uint8_t*>(info_str.data()), info_str.size());

    size_t len = out_len;
    EVP_PKEY_derive(ctx, out.data(), &len);
    EVP_PKEY_CTX_free(ctx);

    return out;
}

bool CarPlayCrypto::chacha20_poly1305_encrypt(
    const uint8_t* key,
    const uint8_t* nonce, size_t nonce_len,
    const uint8_t* aad, size_t aad_len,
    const uint8_t* plaintext, size_t plaintext_len,
    uint8_t* ciphertext, uint8_t* tag)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outl = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) > 0 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce_len, nullptr) > 0 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) > 0) {

        if (aad && aad_len > 0) {
            EVP_EncryptUpdate(ctx, nullptr, &outl, aad, aad_len);
        }

        if (EVP_EncryptUpdate(ctx, ciphertext, &outl, plaintext, plaintext_len) > 0 &&
            EVP_EncryptFinal_ex(ctx, ciphertext + outl, &outl) > 0 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) > 0) {
            ok = true;
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool CarPlayCrypto::chacha20_poly1305_decrypt(
    const uint8_t* key,
    const uint8_t* nonce, size_t nonce_len,
    const uint8_t* aad, size_t aad_len,
    const uint8_t* ciphertext, size_t ciphertext_len,
    const uint8_t* tag,
    uint8_t* plaintext)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outl = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) > 0 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce_len, nullptr) > 0 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) > 0) {

        if (aad && aad_len > 0) {
            EVP_DecryptUpdate(ctx, nullptr, &outl, aad, aad_len);
        }

        if (EVP_DecryptUpdate(ctx, plaintext, &outl, ciphertext, ciphertext_len) > 0) {
            // Set expected tag
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                const_cast<uint8_t*>(tag));
            ok = (EVP_DecryptFinal_ex(ctx, plaintext + outl, &outl) > 0);
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

void CarPlayCrypto::ed25519_sign(
    const uint8_t* secret_key,
    const uint8_t* message, size_t message_len,
    uint8_t* signature)
{
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, secret_key, 32);
    if (!pkey) return;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    size_t sig_len = 64;

    EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey);
    EVP_DigestSign(md_ctx, signature, &sig_len, message, message_len);

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
}

bool CarPlayCrypto::ed25519_verify(
    const uint8_t* public_key,
    const uint8_t* message, size_t message_len,
    const uint8_t* signature)
{
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key, 32);
    if (!pkey) return false;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    bool ok = false;

    if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) > 0) {
        ok = (EVP_DigestVerify(md_ctx, signature, 64, message, message_len) > 0);
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

void CarPlayCrypto::x25519_keygen(uint8_t* public_key, uint8_t* private_key)
{
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return;

    if (EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0) {
        size_t len = 32;
        EVP_PKEY_get_raw_private_key(pkey, private_key, &len);
        EVP_PKEY_get_raw_public_key(pkey, public_key, &len);
    }

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
}

void CarPlayCrypto::x25519_shared_secret(
    const uint8_t* our_private,
    const uint8_t* their_public,
    uint8_t* shared)
{
    EVP_PKEY* our_key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr, our_private, 32);
    EVP_PKEY* their_key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, their_public, 32);

    if (!our_key || !their_key) {
        EVP_PKEY_free(our_key);
        EVP_PKEY_free(their_key);
        return;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(our_key, nullptr);
    if (ctx) {
        EVP_PKEY_derive_init(ctx);
        EVP_PKEY_derive_set_peer(ctx, their_key);
        size_t len = 32;
        EVP_PKEY_derive(ctx, shared, &len);
        EVP_PKEY_CTX_free(ctx);
    }

    EVP_PKEY_free(our_key);
    EVP_PKEY_free(their_key);
}

std::string CarPlayCrypto::generate_pin()
{
    // Generate a 4-digit PIN (0000–9999)
    uint32_t val = 0;
    RAND_bytes(reinterpret_cast<uint8_t*>(&val), sizeof(val));
    val %= 10000;

    char buf[8];
    snprintf(buf, sizeof(buf), "%04u", val);
    return std::string(buf);
}

} // namespace openautolink
