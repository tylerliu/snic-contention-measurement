#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_BUFFER_SIZE 2048
#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282 /* TODO make it a CLI argument */

int main(int argc, char *argv[]) {
    int port = argc == 1 ? PARTIAL_DECRYPTION_TRAFFIC_PORT : atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[MAX_BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Listening for UDP packets on port %d...\n", port);

    while (1) {
        socklen_t len = sizeof(client_addr);
        ssize_t n = recvfrom(sockfd, buffer, MAX_BUFFER_SIZE, 0,
                             (struct sockaddr *)&client_addr, &len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Received %zd bytes from %s:%d\n", n, client_ip, ntohs(client_addr.sin_port));
    }

    close(sockfd);
    return EXIT_SUCCESS;
}