#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282 /* TODO make it a CLI argument */

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <Destination IP> [<Port>]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *dest_ip = argv[1];
    int dest_port = argc == 3 ? atoi(argv[2]) : PARTIAL_DECRYPTION_TRAFFIC_PORT;

    // Create a UDP socket.
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return EXIT_FAILURE;
    }

    // Set up destination address structure.
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) <= 0) {
        perror("Invalid destination address");
        close(sockfd);
        return EXIT_FAILURE;
    }

    // Seed the random number generator.
    srand(time(NULL));

    // Continuously generate and send random UDP packets.
    while (1) {
        // Choose a random packet size between 0 to 80 AES-128 Blocks (besides 12-byte IV).
        int packet_size = (rand() % 80) * 16 + 12; 
        char *packet = malloc(packet_size);
        if (packet == NULL) {
            perror("malloc failed");
            close(sockfd);
            return EXIT_FAILURE;
        }

        // Fill the packet with random data.
        for (int i = 0; i < packet_size; i++) {
            packet[i] = rand() % 256;
        }

        // Send the UDP packet.
        ssize_t sent_bytes = sendto(sockfd, packet, packet_size, 0,
                                    (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (sent_bytes < 0) {
            perror("sendto failed");
        } else {
            printf("Sent %zd bytes to %s:%d\n", sent_bytes, dest_ip, dest_port);
        }

        free(packet);

        // Optional: sleep for 100 milliseconds to avoid flooding the network.
        usleep(100000);
    }

    // Although the loop above is infinite, if you ever decide to exit:
    close(sockfd);
    return EXIT_SUCCESS;
}