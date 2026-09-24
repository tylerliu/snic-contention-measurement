#include "dpdk_offloads.h"
#include <doca_log.h>
#include <stdexcept>
#include <cstdint>

DOCA_LOG_REGISTER(LITEFS_OFFLOADS);

// CompressDevice implementation
CompressDevice::CompressDevice(uint16_t device_id) : dev_id_(UINT16_MAX), nb_queue_pairs_(1) {
    const uint8_t nb = rte_compressdev_count();
    if (device_id >= nb) {
        throw std::runtime_error("Invalid compress device id");
    }

    const int socket_id = rte_compressdev_socket_id((uint8_t)device_id);
    struct rte_compressdev_config cfg;
    cfg.socket_id = socket_id;
    cfg.nb_queue_pairs = nb_queue_pairs_;
    cfg.max_nb_priv_xforms = 0; // driver default
    cfg.max_nb_streams = 0;     // driver default
    if (rte_compressdev_configure((uint8_t)device_id, &cfg) < 0) {
        throw std::runtime_error("Failed to configure compressdev " + std::to_string(device_id));
    }
    // Setup all queue pairs
    for (uint16_t qp = 0; qp < nb_queue_pairs_; ++qp) {
        if (rte_compressdev_queue_pair_setup((uint8_t)device_id, qp, 128, socket_id) < 0) {
            throw std::runtime_error("Failed to setup queue pair for compressdev " + std::to_string(device_id));
        }
    }
    if (rte_compressdev_start((uint8_t)device_id) < 0) {
        throw std::runtime_error("Failed to start compressdev " + std::to_string(device_id));
    }

    dev_id_ = device_id;
}

CompressDevice::CompressDevice(uint16_t device_id, uint16_t nb_queue_pairs, uint32_t max_inflight_ops)
    : dev_id_(UINT16_MAX), nb_queue_pairs_(nb_queue_pairs) {
    const uint8_t nb = rte_compressdev_count();
    if (device_id >= nb) {
        throw std::runtime_error("Invalid compress device id");
    }
    const int socket_id = rte_compressdev_socket_id((uint8_t)device_id);
    struct rte_compressdev_config cfg;
    cfg.socket_id = socket_id;
    cfg.nb_queue_pairs = nb_queue_pairs_;
    cfg.max_nb_priv_xforms = 0;
    cfg.max_nb_streams = 0;
    if (rte_compressdev_configure((uint8_t)device_id, &cfg) < 0) {
        throw std::runtime_error("Failed to configure compressdev " + std::to_string(device_id));
    }
    for (uint16_t qp = 0; qp < nb_queue_pairs_; ++qp) {
        if (rte_compressdev_queue_pair_setup((uint8_t)device_id, qp, max_inflight_ops, socket_id) < 0) {
            throw std::runtime_error("Failed to setup queue pair for compressdev " + std::to_string(device_id));
        }
    }
    if (rte_compressdev_start((uint8_t)device_id) < 0) {
        throw std::runtime_error("Failed to start compressdev " + std::to_string(device_id));
    }
    dev_id_ = device_id;
}

CompressDevice::~CompressDevice() {
    if (dev_id_ != UINT16_MAX) {
        DOCA_LOG_INFO("Stopping compression device %u", dev_id_);
        rte_compressdev_stop((uint8_t)dev_id_);
    }
}

// CryptoDevice implementation
CryptoDevice::CryptoDevice(uint16_t device_id) : CryptoDevice(device_id, 1, CRYPTO_DEFAULT_NB_DESCRIPTORS) {}

CryptoDevice::CryptoDevice(uint16_t device_id, uint16_t nb_queue_pairs, uint16_t nb_descriptors)
    : dev_id_(device_id), nb_queue_pairs_(nb_queue_pairs) {
    const uint16_t nb = rte_cryptodev_count();
    if (device_id >= nb) {
        throw std::runtime_error("Invalid crypto device id");
    }
    const int socket_id = rte_socket_id();
    struct rte_cryptodev_config conf = {
        .socket_id = socket_id,
        .nb_queue_pairs = nb_queue_pairs_,
        .ff_disable = RTE_CRYPTODEV_FF_SECURITY,
    };
    if (rte_cryptodev_configure(dev_id_, &conf) < 0) {
        throw std::runtime_error("Failed to configure cryptodev " + std::to_string(device_id));
    }
    struct rte_cryptodev_qp_conf qp_conf = {
        .nb_descriptors = nb_descriptors,
        .mp_session = nullptr,
    };
    for (uint16_t qp = 0; qp < nb_queue_pairs_; ++qp) {
        if (rte_cryptodev_queue_pair_setup(dev_id_, qp, &qp_conf, socket_id) < 0) {
            throw std::runtime_error("Failed to setup queue pair for cryptodev " + std::to_string(device_id));
        }
    }
    if (rte_cryptodev_start(dev_id_) < 0) {
        throw std::runtime_error("Failed to start cryptodev " + std::to_string(device_id));
    }
}

CryptoDevice::~CryptoDevice() {
    if (dev_id_ != UINT16_MAX) {
        DOCA_LOG_INFO("Stopping crypto device %u", dev_id_);
        rte_cryptodev_stop((uint8_t)dev_id_);
    }
}

// Static discovery helpers
uint16_t CompressDevice::count() {
    return rte_compressdev_count();
}

uint16_t CryptoDevice::count() {
    return rte_cryptodev_count();
}

uint8_t CryptoDevice::socket_id() const {
    return dev_id_;
}

struct rte_mempool *CryptoDevice::create_crypto_op_pool(const char *name, uint32_t nb_elts,
                                                        uint32_t cache_size, uint16_t private_area_len) const {
    return rte_crypto_op_pool_create(name,
                                     RTE_CRYPTO_OP_TYPE_SYMMETRIC,
                                     nb_elts,
                                     cache_size,
                                     private_area_len,
                                     socket_id());
}

struct rte_mempool *CryptoDevice::create_session_pool(const char *name, uint32_t nb_elts,
                                                      uint32_t cache_size) const {
    const uint16_t priv = rte_cryptodev_sym_get_private_session_size((uint8_t)dev_id_);
    if (priv == 0) {
        return nullptr;
    }
    // elt_size may be ignored if smaller than minimum, pass priv to be safe
    return rte_cryptodev_sym_session_pool_create(name, nb_elts, /*elt_size*/priv, cache_size, priv, socket_id());
}

// AesGcmDevice helpers
struct rte_mempool *AesGcmDevice::create_crypto_op_pool(const char *name, uint32_t nb_elts,
                                                        uint32_t cache_size) const {
    return CryptoDevice::create_crypto_op_pool(name, nb_elts, cache_size, kIvLen);
}

struct rte_cryptodev_sym_session *AesGcmDevice::create_decrypt_session(struct rte_mempool *session_pool,
                                                                       const uint8_t *key, uint16_t key_len,
                                                                       uint16_t digest_len, uint16_t aad_len) const {
    struct rte_crypto_sym_xform xform = {};
    xform.type = RTE_CRYPTO_SYM_XFORM_AEAD;
    xform.next = NULL;
    xform.aead.op = RTE_CRYPTO_AEAD_OP_DECRYPT;
    xform.aead.algo = RTE_CRYPTO_AEAD_AES_GCM;
    xform.aead.key.data = const_cast<uint8_t *>(key);
    xform.aead.key.length = key_len;
    xform.aead.iv.offset = sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op);
    xform.aead.iv.length = kIvLen;
    xform.aead.aad_length = aad_len;
    xform.aead.digest_length = digest_len;
    return (struct rte_cryptodev_sym_session *)rte_cryptodev_sym_session_create((uint8_t)get_dev_id(), &xform, session_pool);
}

struct rte_cryptodev_sym_session *AesGcmDevice::create_encrypt_session(struct rte_mempool *session_pool,
                                                                       const uint8_t *key, uint16_t key_len,
                                                                       uint16_t digest_len, uint16_t aad_len) const {
    struct rte_crypto_sym_xform xform = {};
    xform.type = RTE_CRYPTO_SYM_XFORM_AEAD;
    xform.next = NULL;
    xform.aead.op = RTE_CRYPTO_AEAD_OP_ENCRYPT;
    xform.aead.algo = RTE_CRYPTO_AEAD_AES_GCM;
    xform.aead.key.data = const_cast<uint8_t *>(key);
    xform.aead.key.length = key_len;
    xform.aead.iv.offset = sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op);
    xform.aead.iv.length = kIvLen;
    xform.aead.aad_length = aad_len;
    xform.aead.digest_length = digest_len;
    return (struct rte_cryptodev_sym_session *)rte_cryptodev_sym_session_create((uint8_t)get_dev_id(), &xform, session_pool);
}

// AesXtsDevice helpers
struct rte_mempool *AesXtsDevice::create_crypto_op_pool(const char *name, uint32_t nb_elts,
                                                        uint32_t cache_size, uint16_t additional_private_area_len) const {
    return CryptoDevice::create_crypto_op_pool(name, nb_elts, cache_size, kIvLen + additional_private_area_len);
}

struct rte_cryptodev_sym_session *AesXtsDevice::create_cipher_session(struct rte_mempool *session_pool,
                                                                      const uint8_t *key, uint16_t key_len,
                                                                      uint16_t data_unit_len,
                                                                      bool encrypt) const {
    struct rte_crypto_sym_xform xform = {};
    xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
    xform.next = NULL;
    xform.cipher.op = encrypt ? RTE_CRYPTO_CIPHER_OP_ENCRYPT : RTE_CRYPTO_CIPHER_OP_DECRYPT;
    xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_XTS;
    xform.cipher.key.data = const_cast<uint8_t *>(key);
    xform.cipher.key.length = key_len; 
    xform.cipher.iv.offset = sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op);
    xform.cipher.iv.length = kIvLen;
    xform.cipher.dataunit_len = data_unit_len;
    return (struct rte_cryptodev_sym_session *)rte_cryptodev_sym_session_create(get_dev_id(), &xform, session_pool);
}
