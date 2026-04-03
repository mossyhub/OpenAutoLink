#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openautolink {

/// HomeKit Pairing v2 cryptography for CarPlay.
///
/// Implements:
///   - SRP-6a pair-setup (first-time pairing with PIN)
///   - X25519 pair-verify (subsequent reconnections)
///   - ChaCha20-Poly1305 transport encryption
///   - HKDF-SHA-512 key derivation
///   - Ed25519 long-term identity keys
///
/// All crypto uses OpenSSL 3.0 (already on the SBC for aasdk).
/// No additional crypto dependencies needed.
///
/// Protocol flow:
///
/// First-time pairing (pair-setup):
///   1. iPhone sends pair-setup M1 (SRP start request)
///   2. Bridge generates 4-digit PIN, sends to car app via OAL
///   3. Bridge responds with M2 (SRP salt + public key B)
///   4. iPhone sends M3 (SRP public key A + proof M1)
///   5. Bridge verifies M1, responds with M4 (proof M2)
///   6. iPhone sends M5 (encrypted LTPK exchange)
///   7. Bridge stores iPhone's LTPK, responds with M6
///
/// Subsequent connections (pair-verify):
///   1. iPhone sends pair-verify M1 (X25519 public key)
///   2. Bridge responds with M2 (encrypted verification)
///   3. iPhone sends M3 (encrypted verification)
///   4. Session keys derived, encrypted channel established
class CarPlayCrypto {
public:
    /// Callback to notify CarPlay session of PIN for display.
    using PinCallback = std::function<void(const std::string& pin)>;

    /// Callback when pairing is complete (success/failure).
    using PairCompleteCallback = std::function<void(bool success)>;

    /// Callback when pair-verify is complete and session keys are ready.
    using VerifyCompleteCallback = std::function<void(bool success)>;

    CarPlayCrypto();
    ~CarPlayCrypto();

    CarPlayCrypto(const CarPlayCrypto&) = delete;
    CarPlayCrypto& operator=(const CarPlayCrypto&) = delete;

    void set_pin_callback(PinCallback cb) { pin_cb_ = std::move(cb); }
    void set_pair_complete_callback(PairCompleteCallback cb) { pair_complete_cb_ = std::move(cb); }
    void set_verify_complete_callback(VerifyCompleteCallback cb) { verify_complete_cb_ = std::move(cb); }

    /// Initialize long-term identity keypair.
    /// Loads from file if exists, generates new one otherwise.
    bool init_identity(const std::string& keys_path = "/var/lib/openautolink/carplay_keys");

    // ── Pair-Setup (SRP-6a) ──────────────────────────────────────

    /// Handle pair-setup request from iPhone.
    /// Returns response TLV data to send back.
    std::vector<uint8_t> handle_pair_setup(const std::vector<uint8_t>& request);

    /// Check if a device is already paired (has stored LTPK).
    bool is_paired() const { return has_peer_ltpk_; }

    // ── Pair-Verify (X25519) ────────────────────────────────────

    /// Handle pair-verify request from iPhone.
    /// Returns response data to send back.
    std::vector<uint8_t> handle_pair_verify(const std::vector<uint8_t>& request);

    // ── Transport Encryption ─────────────────────────────────────

    /// Encrypt data for sending to iPhone.
    std::vector<uint8_t> encrypt(const uint8_t* data, size_t len);

    /// Decrypt data received from iPhone.
    /// Returns empty vector on failure (auth tag mismatch).
    std::vector<uint8_t> decrypt(const uint8_t* data, size_t len);

    /// Check if transport encryption is established.
    bool is_encrypted() const { return encryption_active_; }

    /// Get the current session read/write keys (for stream decryption).
    const std::array<uint8_t, 32>& read_key() const { return read_key_; }
    const std::array<uint8_t, 32>& write_key() const { return write_key_; }

private:
    // ── TLV8 encoding/decoding ───────────────────────────────────
    // Apple's HomeKit uses TLV8 (Type-Length-Value with 8-bit length)
    // for pair-setup/verify message framing.

    struct Tlv8 {
        uint8_t type;
        std::vector<uint8_t> value;
    };

    static std::vector<Tlv8> parse_tlv8(const uint8_t* data, size_t len);
    static std::vector<uint8_t> build_tlv8(const std::vector<Tlv8>& items);
    static const std::vector<uint8_t>* find_tlv(const std::vector<Tlv8>& items, uint8_t type);

    // TLV types used in HomeKit pairing
    static constexpr uint8_t kTlvType_Method      = 0x00;
    static constexpr uint8_t kTlvType_Identifier   = 0x01;
    static constexpr uint8_t kTlvType_Salt         = 0x02;
    static constexpr uint8_t kTlvType_PublicKey     = 0x03;
    static constexpr uint8_t kTlvType_Proof         = 0x04;
    static constexpr uint8_t kTlvType_EncryptedData = 0x05;
    static constexpr uint8_t kTlvType_State         = 0x06;
    static constexpr uint8_t kTlvType_Error         = 0x07;
    static constexpr uint8_t kTlvType_Signature     = 0x0A;

    // ── SRP-6a state ────────────────────────────────────────────

    enum class PairSetupState {
        Idle,
        WaitingM3,   // Sent M2 (salt+B), waiting for M3 (A+proof)
        WaitingM5,   // Sent M4 (proof), waiting for M5 (encrypted exchange)
        Complete
    };

    std::vector<uint8_t> handle_pair_setup_m1(const std::vector<Tlv8>& request_tlvs);
    std::vector<uint8_t> handle_pair_setup_m3(const std::vector<Tlv8>& request_tlvs);
    std::vector<uint8_t> handle_pair_setup_m5(const std::vector<Tlv8>& request_tlvs);

    PairSetupState pair_setup_state_ = PairSetupState::Idle;
    std::string setup_pin_;  // 4-digit PIN for current pairing

    // SRP-6a parameters (stored during M1→M3 exchange)
    std::vector<uint8_t> srp_salt_;        // 16 bytes random salt
    std::vector<uint8_t> srp_b_;           // Server private key
    std::vector<uint8_t> srp_B_;           // Server public key
    std::vector<uint8_t> srp_v_;           // Verifier
    std::vector<uint8_t> srp_session_key_; // Shared session key (K)

    // ── Pair-Verify state ────────────────────────────────────────

    enum class PairVerifyState {
        Idle,
        WaitingM3,
        Complete
    };

    std::vector<uint8_t> handle_pair_verify_m1(const std::vector<Tlv8>& request_tlvs);
    std::vector<uint8_t> handle_pair_verify_m3(const std::vector<Tlv8>& request_tlvs);

    PairVerifyState pair_verify_state_ = PairVerifyState::Idle;
    std::array<uint8_t, 32> verify_secret_{};        // X25519 shared secret
    std::array<uint8_t, 32> verify_our_pubkey_{};     // Our ephemeral X25519 public key
    std::array<uint8_t, 32> verify_our_privkey_{};    // Our ephemeral X25519 private key
    std::array<uint8_t, 32> verify_peer_pubkey_{};    // iPhone's ephemeral X25519 public key
    std::vector<uint8_t> verify_session_key_;         // Derived session key for encryption

    // ── Long-term identity ────────────────────────────────────────

    std::array<uint8_t, 32> ltsk_{};   // Long-term Ed25519 secret key
    std::array<uint8_t, 32> ltpk_{};   // Long-term Ed25519 public key
    bool identity_initialized_ = false;

    // Peer (iPhone) long-term public key — stored after first pairing
    std::array<uint8_t, 32> peer_ltpk_{};
    std::string peer_identifier_;
    bool has_peer_ltpk_ = false;

    // ── Transport encryption state ───────────────────────────────

    std::array<uint8_t, 32> read_key_{};   // Decrypt incoming from iPhone
    std::array<uint8_t, 32> write_key_{};  // Encrypt outgoing to iPhone
    uint64_t read_nonce_ = 0;
    uint64_t write_nonce_ = 0;
    bool encryption_active_ = false;

    std::string keys_path_;  // Path to stored pairing keys

    // ── Crypto helpers ──────────────────────────────────────────

    static std::vector<uint8_t> hkdf_sha512(const uint8_t* ikm, size_t ikm_len,
                                             const std::string& salt_str,
                                             const std::string& info_str,
                                             size_t out_len);

    static bool chacha20_poly1305_encrypt(const uint8_t* key,
                                          const uint8_t* nonce, size_t nonce_len,
                                          const uint8_t* aad, size_t aad_len,
                                          const uint8_t* plaintext, size_t plaintext_len,
                                          uint8_t* ciphertext, uint8_t* tag);

    static bool chacha20_poly1305_decrypt(const uint8_t* key,
                                          const uint8_t* nonce, size_t nonce_len,
                                          const uint8_t* aad, size_t aad_len,
                                          const uint8_t* ciphertext, size_t ciphertext_len,
                                          const uint8_t* tag,
                                          uint8_t* plaintext);

    static void ed25519_sign(const uint8_t* secret_key,
                             const uint8_t* message, size_t message_len,
                             uint8_t* signature);

    static bool ed25519_verify(const uint8_t* public_key,
                               const uint8_t* message, size_t message_len,
                               const uint8_t* signature);

    static void x25519_keygen(uint8_t* public_key, uint8_t* private_key);
    static void x25519_shared_secret(const uint8_t* our_private,
                                     const uint8_t* their_public,
                                     uint8_t* shared);

    static std::string generate_pin();

    PinCallback pin_cb_;
    PairCompleteCallback pair_complete_cb_;
    VerifyCompleteCallback verify_complete_cb_;

    bool save_keys();
    bool load_keys();
};

} // namespace openautolink
