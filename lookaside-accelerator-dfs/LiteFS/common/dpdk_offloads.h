#ifndef LITEFS_DPDK_OFFLOADS_H
#define LITEFS_DPDK_OFFLOADS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <rte_compressdev.h>
#include <rte_cryptodev.h>
#include <rte_mempool.h>
#include <rte_crypto.h>

#ifdef __cplusplus
}
#endif

#define CRYPTO_DEFAULT_NB_DESCRIPTORS 128

/**
 * @brief An RAII wrapper for a DPDK compression device.
 */
class CompressDevice {
public:
    static constexpr uint32_t kMaxInflightOps = 128;
    /**
     * @brief Initialize a compress device by device id.
     * @param device_id DPDK compress device id (0..count-1).
     * @throws std::runtime_error on failure.
     */
    explicit CompressDevice(uint16_t device_id);
    /**
     * @brief Initialize a compress device with explicit queue configuration.
     * @param device_id DPDK compress device id (0..count-1).
     * @param nb_queue_pairs Number of queue pairs to configure.
     * @param max_inflight_ops Max inflight ops per queue pair.
     */
    CompressDevice(uint16_t device_id, uint16_t nb_queue_pairs, uint32_t max_inflight_ops);
    ~CompressDevice();

    uint16_t get_dev_id() const { return dev_id_; }
    uint16_t get_nb_queue_pairs() const { return nb_queue_pairs_; }

    /**
     * @brief Return number of available compress devices.
     */
    static uint16_t count();

    CompressDevice(const CompressDevice&) = delete;
    CompressDevice& operator=(const CompressDevice&) = delete;
    CompressDevice(CompressDevice&&) = delete;
    CompressDevice& operator=(CompressDevice&&) = delete;

private:
    uint16_t dev_id_;
    uint16_t nb_queue_pairs_;
};

/**
 * @brief An RAII wrapper for a DPDK crypto device.
 */
class CryptoDevice {
public:
    static constexpr uint16_t kDefaultNbDescriptors = 128;
    /**
     * @brief Initialize a crypto device by device id.
     * @param device_id DPDK crypto device id (0..count-1).
     * @throws std::runtime_error on failure.
     */
    explicit CryptoDevice(uint16_t device_id);
    /**
     * @brief Initialize a crypto device with explicit queue configuration.
     * @param device_id DPDK crypto device id (0..count-1).
     * @param nb_queue_pairs Number of queue pairs to configure.
     * @param nb_descriptors Number of descriptors per queue pair.
     */
    CryptoDevice(uint16_t device_id, uint16_t nb_queue_pairs, uint16_t nb_descriptors);
    ~CryptoDevice();

    uint16_t get_dev_id() const { return dev_id_; }
    uint16_t get_nb_queue_pairs() const { return nb_queue_pairs_; }

    /**
     * @brief Return number of available crypto devices.
     */
    static uint16_t count();

    /**
     * @brief Convenience: return NUMA socket id of this device.
     */
    uint8_t socket_id() const;

    /**
     * @brief Create a crypto operation pool appropriate for this device.
     * @param name unique mempool name
     * @param nb_elts number of ops
     * @param cache_size per-lcore cache size
     * @param private_area_len per-op private area size (e.g., IV size)
     */
    struct rte_mempool *create_crypto_op_pool(const char *name, uint32_t nb_elts,
                                              uint32_t cache_size, uint16_t private_area_len) const;

    /**
     * @brief Create a symmetric session pool sized for this device.
     * @param name unique mempool name
     * @param nb_elts number of sessions
     * @param cache_size per-lcore cache size
     */
    struct rte_mempool *create_session_pool(const char *name, uint32_t nb_elts,
                                            uint32_t cache_size) const;

    CryptoDevice(const CryptoDevice&) = delete;
    CryptoDevice& operator=(const CryptoDevice&) = delete;
    CryptoDevice(CryptoDevice&&) = delete;
    CryptoDevice& operator=(CryptoDevice&&) = delete;

private:
    uint16_t dev_id_;
    uint16_t nb_queue_pairs_;
};

/**
 * @brief AES-GCM specialized crypto device.
 */
class AesGcmDevice : public CryptoDevice {
public:
    static constexpr uint16_t kIvLen = 12; // bytes
    using CryptoDevice::CryptoDevice; // inherit constructors

    struct rte_mempool *create_crypto_op_pool(const char *name, uint32_t nb_elts,
                                              uint32_t cache_size) const;

    struct rte_cryptodev_sym_session *create_decrypt_session(struct rte_mempool *session_pool,
                                                             const uint8_t *key, uint16_t key_len,
                                                             uint16_t digest_len, uint16_t aad_len) const;

    struct rte_cryptodev_sym_session *create_encrypt_session(struct rte_mempool *session_pool,
                                                             const uint8_t *key, uint16_t key_len,
                                                             uint16_t digest_len, uint16_t aad_len) const;
};

/**
 * @brief AES-XTS specialized crypto device.
 */
class AesXtsDevice : public CryptoDevice {
public:
    static constexpr uint16_t kIvLen = 16; // bytes (tweak)
    using CryptoDevice::CryptoDevice; // inherit constructors

    struct rte_mempool *create_crypto_op_pool(const char *name, uint32_t nb_elts,
                                              uint32_t cache_size, uint16_t additional_private_area_len) const;

    struct rte_cryptodev_sym_session *create_cipher_session(struct rte_mempool *session_pool,
                                                            const uint8_t *key, uint16_t key_len,
                                                            uint16_t data_unit_len,
                                                            bool encrypt) const;
};

#endif /* LITEFS_DPDK_OFFLOADS_H */
