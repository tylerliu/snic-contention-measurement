#ifndef ESP_GENERATOR_SOURCE
#define ESP_GENERATOR_SOURCE "dpdk_esp_gen.c"
#endif
#define main generator_main
#include ESP_GENERATOR_SOURCE
#undef main
#include <assert.h>

int main(void) {
    const unsigned sizes[] = {1, 1414, 1472};
    const uint32_t seqs[] = {1, 2, UINT32_MAX};
    unsigned tests = 0;
    struct in_addr src, dst;
    inet_aton("172.16.1.20", &src);
    inet_aton("172.16.1.128", &dst);
    struct rte_ether_addr mac = {{0, 1, 2, 3, 4, 5}};
    unsigned char key[32];
    memset(key, 0xa5, sizeof(key));
    for (unsigned bits = 128; bits <= 256; bits += 128) {
        OSSL_LIB_CTX *lib = OSSL_LIB_CTX_new();
        EVP_CIPHER *cipher = EVP_CIPHER_fetch(lib, bits == 128 ? "AES-128-GCM" : "AES-256-GCM", NULL);
        EVP_CIPHER_CTX *old = EVP_CIPHER_CTX_new(), *now = EVP_CIPHER_CTX_new();
        assert(lib && cipher && old && now);
        assert(EVP_EncryptInit_ex(old, bits == 128 ? EVP_aes_128_gcm() : EVP_aes_256_gcm(), NULL, key, NULL) == 1);
        assert(EVP_EncryptInit_ex(now, cipher, NULL, key, NULL) == 1);
        for (unsigned ivmode = 0; ivmode < 2; ivmode++)
        for (unsigned sz = 0; sz < 3; sz++)
        for (unsigned sn = 0; sn < 3; sn++) {
            unsigned char a[2048] = {0}, b[2048] = {0};
            struct rte_mbuf ma, mb;
            memset(&ma, 0, sizeof(ma)); memset(&mb, 0, sizeof(mb));
            ma.buf_addr = a; mb.buf_addr = b;
            global_payload_size = sizes[sz];
            uint64_t iv = ivmode ? UINT64_C(0x1234567890abcdef) : 0;
            assert(fill_mbuf_with_esp_tunnel_packet(&ma, old, src, dst, dst,
                &mac, &mac, 12345, 3333, seqs[sn], 0x2001, 0x22334455, iv) == 0);
            assert(fill_mbuf_with_esp_tunnel_packet(&mb, now, src, dst, dst,
                &mac, &mac, 12345, 3333, seqs[sn], 0x2001, 0x22334455, iv) == 0);
            assert(ma.pkt_len == mb.pkt_len);
            assert(memcmp(a, b, ma.pkt_len) == 0);
            assert(ma.ol_flags == mb.ol_flags);
            EVP_CIPHER_CTX *dec = EVP_CIPHER_CTX_new();
            unsigned char nonce[12], plain[2048];
            uint32_t salt_be = rte_cpu_to_be_32(0x22334455);
            memcpy(nonce, &salt_be, 4); memcpy(nonce + 4, b + 42, 8);
            int out = 0, len = 0;
            assert(dec);
            assert(EVP_DecryptInit_ex(dec, bits == 128 ? EVP_aes_128_gcm() : EVP_aes_256_gcm(), NULL, key, nonce) == 1);
            assert(EVP_DecryptUpdate(dec, NULL, &len, b + 34, 8) == 1);
            assert(EVP_DecryptUpdate(dec, plain, &out, b + 50, mb.pkt_len - 66) == 1);
            assert(EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_GCM_SET_TAG, 16, b + mb.pkt_len - 16) == 1);
            assert(EVP_DecryptFinal_ex(dec, plain + out, &len) == 1);
            assert(rte_ipv4_cksum((struct rte_ipv4_hdr *)plain) == 0);
            for (unsigned j = 0; j < sizes[sz]; j++) assert(plain[28 + j] == 0xa5);
            EVP_CIPHER_CTX_free(dec);
            tests++;
        }
        EVP_CIPHER_CTX_free(old); EVP_CIPHER_CTX_free(now);
        EVP_CIPHER_free(cipher); OSSL_LIB_CTX_free(lib);
    }
    printf("PASS: %u byte-identical ESP packet cases, shared vs private cipher\n", tests);
    return 0;
}
