#ifndef LITEFS_TCP_HELPER_H
#define LITEFS_TCP_HELPER_H

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

// Key/Value helpers for tagged multi-part messages
struct TcpKvItem {
    std::string key;
    std::vector<uint8_t> value;
};

// A simple wrapper for a listening server socket
class TcpServer {
public:
    explicit TcpServer(uint16_t port);
    ~TcpServer();

    // Waits for a client to connect, reads its identity, and returns the socket fd.
    int Accept(std::string& client_identity);

    // Store/read K/V associated with a connected identity
    void ingest_kv_batch_from_socket(int sock, const std::string& identity);
    void send_kv_batch_to_identity(const std::string& identity, const std::vector<TcpKvItem>& items);
    bool get_kv(const std::string& identity, const std::string& key, std::vector<uint8_t>& out_value) const;
    const std::unordered_map<std::string, std::vector<uint8_t>>& get_kv_map(const std::string& identity) const;
    int get_client_socket(const std::string& identity) const;
    void close_client(const std::string& identity);

private:
    int listen_sock_;
    std::unordered_map<std::string, int> identity_to_sock_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<uint8_t>>> identity_to_kvs_;
};

// Client function to connect and send identity
int tcp_client_connect(const std::string& server_ip, uint16_t port, const std::string& identity);

// Simple stateful TCP client with K/V cache
class TcpClient {
public:
    TcpClient(): sock_(-1) {}
    ~TcpClient() { if (sock_ >= 0) close(); }

    void connect(const std::string& server_ip, uint16_t port, const std::string& identity);
    void send_kv_batch(const std::vector<TcpKvItem>& items);
    void read_kv_batch();
    bool get_kv(const std::string& key, std::vector<uint8_t>& out_value) const;
    void set_kv(const std::string& key, const std::vector<uint8_t>& value);
    int sock() const { return sock_; }
    void close();

private:
    int sock_;
    std::string identity_;
    std::unordered_map<std::string, std::vector<uint8_t>> kvs_;
};

// Batch (count-prefixed) K/V exchange
void tcp_write_kv_batch(int sock, const std::vector<TcpKvItem>& items);
void tcp_read_kv_batch(int sock, std::vector<TcpKvItem>& out_items);


#endif // LITEFS_TCP_HELPER_H
