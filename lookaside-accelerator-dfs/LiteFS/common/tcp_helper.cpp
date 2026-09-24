#include "tcp_helper.h"

#include <stdexcept>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// constexpr int kMaxIdentityLen = 64; // unused

// Internal forward declarations for helpers used before their definitions
static void tcp_write_prefixed(int sock, const void* data, uint32_t len);
static void tcp_read_prefixed(int sock, std::vector<uint8_t>& out_data);

// Helper to ensure all bytes are written (internal)
static void tcp_write(int sock, const void* data, size_t len) {
    size_t bytes_written = 0;
    while (bytes_written < len) {
        ssize_t res = write(sock, (const char*)data + bytes_written, len - bytes_written);
        if (res < 0) throw std::runtime_error("TCP write failed");
        bytes_written += res;
    }
}

// Helper to ensure all bytes are read (internal)
static void tcp_read(int sock, void* data, size_t len) {
    size_t bytes_read = 0;
    while (bytes_read < len) {
        ssize_t res = read(sock, (char*)data + bytes_read, len - bytes_read);
        if (res <= 0) throw std::runtime_error("TCP read failed or connection closed prematurely");
        bytes_read += res;
    }
}

TcpServer::TcpServer(uint16_t port) {
    listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_ < 0) throw std::runtime_error("Failed to create listen socket");

    int enable = 1;
    if (setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
    }

    struct sockaddr_in serv_addr = {};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        throw std::runtime_error("Failed to bind listen socket");
    }

    if (listen(listen_sock_, 2) < 0) { // Listen for 2 clients (hostfs, nic_tester)
        throw std::runtime_error("Failed to listen on socket");
    }
    std::cout << "[TcpServer] Listening on port " << port << std::endl;
}

TcpServer::~TcpServer() {
    close(listen_sock_);
}

int TcpServer::Accept(std::string& client_identity) {
    int client_sock = accept(listen_sock_, nullptr, nullptr);
    if (client_sock < 0) {
        throw std::runtime_error("Failed to accept client connection");
    }

    std::vector<uint8_t> identity_buf;
    tcp_read_prefixed(client_sock, identity_buf);
    client_identity = std::string(identity_buf.begin(), identity_buf.end());
    identity_to_sock_[client_identity] = client_sock;
    return client_sock;
}

int tcp_client_connect(const std::string& server_ip, uint16_t port, const std::string& identity) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) throw std::runtime_error("Failed to create client socket");

    struct sockaddr_in serv_addr = {};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        throw std::runtime_error("Invalid server IP address");
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        throw std::runtime_error("Failed to connect to server");
    }

    // Send identity first
    tcp_write_prefixed(sock, identity.c_str(), identity.length());

    return sock;
}

void TcpServer::ingest_kv_batch_from_socket(int sock, const std::string& identity) {
    std::vector<TcpKvItem> items;
    tcp_read_kv_batch(sock, items);
    auto& kv = identity_to_kvs_[identity];
    for (auto& it : items) kv[it.key] = std::move(it.value);
}

void TcpServer::send_kv_batch_to_identity(const std::string& identity, const std::vector<TcpKvItem>& items) {
    auto it = identity_to_sock_.find(identity);
    if (it == identity_to_sock_.end()) throw std::runtime_error("Unknown identity");
    tcp_write_kv_batch(it->second, items);
}

bool TcpServer::get_kv(const std::string& identity, const std::string& key, std::vector<uint8_t>& out_value) const {
    auto id_it = identity_to_kvs_.find(identity);
    if (id_it == identity_to_kvs_.end()) return false;
    auto kv_it = id_it->second.find(key);
    if (kv_it == id_it->second.end()) return false;
    out_value = kv_it->second;
    return true;
}

const std::unordered_map<std::string, std::vector<uint8_t>>& TcpServer::get_kv_map(const std::string& identity) const {
    static const std::unordered_map<std::string, std::vector<uint8_t>> empty;
    auto it = identity_to_kvs_.find(identity);
    if (it == identity_to_kvs_.end()) return empty;
    return it->second;
}

int TcpServer::get_client_socket(const std::string& identity) const {
    auto it = identity_to_sock_.find(identity);
    if (it == identity_to_sock_.end()) return -1;
    return it->second;
}

void TcpServer::close_client(const std::string& identity) {
    auto it = identity_to_sock_.find(identity);
    if (it == identity_to_sock_.end()) return;
    ::close(it->second);
    identity_to_sock_.erase(it);
}

void TcpClient::connect(const std::string& server_ip, uint16_t port, const std::string& identity) {
    sock_ = tcp_client_connect(server_ip, port, identity);
    identity_ = identity;
}

void TcpClient::send_kv_batch(const std::vector<TcpKvItem>& items) {
    tcp_write_kv_batch(sock_, items);
}

void TcpClient::read_kv_batch() {
    std::vector<TcpKvItem> items;
    tcp_read_kv_batch(sock_, items);
    for (auto& it : items) kvs_[it.key] = std::move(it.value);
}

bool TcpClient::get_kv(const std::string& key, std::vector<uint8_t>& out_value) const {
    auto it = kvs_.find(key);
    if (it == kvs_.end()) return false;
    out_value = it->second;
    return true;
}

void TcpClient::set_kv(const std::string& key, const std::vector<uint8_t>& value) {
    kvs_[key] = value;
}

void TcpClient::close() {
    if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
}

static void tcp_write_prefixed(int sock, const void* data, uint32_t len) {
    uint32_t net_len = htonl(len);
    tcp_write(sock, &net_len, sizeof(net_len));
    tcp_write(sock, data, len);
}

static void tcp_read_prefixed(int sock, std::vector<uint8_t>& out_data) {
    uint32_t net_len;
    tcp_read(sock, &net_len, sizeof(net_len));
    uint32_t len = ntohl(net_len);

    out_data.resize(len);
    tcp_read(sock, out_data.data(), len);
}

static void tcp_write_kv(int sock, const std::string& key, const void* value, uint32_t value_len) {
    // key
    tcp_write_prefixed(sock, key.data(), static_cast<uint32_t>(key.size()));
    // value
    tcp_write_prefixed(sock, value, value_len);
}

static void tcp_read_kv(int sock, std::string& key_out, std::vector<uint8_t>& value_out) {
    std::vector<uint8_t> k;
    tcp_read_prefixed(sock, k);
    key_out.assign(reinterpret_cast<const char*>(k.data()), k.size());
    tcp_read_prefixed(sock, value_out);
}

void tcp_write_kv_batch(int sock, const std::vector<TcpKvItem>& items) {
    uint32_t count = static_cast<uint32_t>(items.size());
    uint32_t net_cnt = htonl(count);
    tcp_write(sock, &net_cnt, sizeof(net_cnt));
    for (const auto& it : items) {
        tcp_write_kv(sock, it.key, it.value.data(), static_cast<uint32_t>(it.value.size()));
    }
}

void tcp_read_kv_batch(int sock, std::vector<TcpKvItem>& out_items) {
    uint32_t net_cnt = 0;
    tcp_read(sock, &net_cnt, sizeof(net_cnt));
    uint32_t count = ntohl(net_cnt);
    out_items.clear();
    out_items.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TcpKvItem item;
        tcp_read_kv(sock, item.key, item.value);
        out_items.push_back(std::move(item));
    }
}
