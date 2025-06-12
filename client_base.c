#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include "protocol.h"

#define SERVER_IP "192.168.6.1"
#define SERVER_PORT 8888
#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 9999

int tcp_sock;
struct sockaddr_in server_addr;

// Thread function declarations
void* udp_listener_thread(void* arg);
void* keep_alive_thread(void* arg);

int main() {
    char buffer[1024];
    pthread_t udp_thread, keepalive_thread;

    // Ask user before connecting
    char choice[8];
    printf("Do you want to join the game? (y/n): ");
    fgets(choice, sizeof(choice), stdin);

    if (choice[0] != 'y' && choice[0] != 'Y') {
        printf("Canceled. Exiting.\n");
        return 0;
    }

    // Create TCP connection to server
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    memset(&(server_addr.sin_zero), 0, 8);

    if (connect(tcp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP connection failed");
        return 1;
    }

    // Receive verification code from server
    TrvMessage msg;
    recv(tcp_sock, &msg, sizeof(msg), 0);
    msg.payload[msg.payload_len] = '\0';
    printf("Server: %s\n", msg.payload);
    
    // User inputs the code
    printf("Enter code: ");
    fgets(buffer, sizeof(buffer), stdin);
    buffer[strcspn(buffer, "\n")] = '\0';
    build_message(&msg, TRV_AUTH_REPLY, 0, buffer);
    send(tcp_sock, &msg, 4 + msg.payload_len, 0);

    // Receive verification result
     recv(tcp_sock, &msg, sizeof(msg), 0);
    msg.payload[msg.payload_len] = '\0';
    printf("Server: %s\n", msg.payload);

      if (msg.type != TRV_AUTH_OK) {
        printf("Exiting.\n");
        close(tcp_sock);
        return 1;
    }

    // Start background threads: UDP listener + TCP Keep-Alive sender
    pthread_create(&udp_thread, NULL, udp_listener_thread, NULL);
    pthread_create(&keepalive_thread, NULL, keep_alive_thread, NULL);

    // Main thread waits for UDP thread to finish
    pthread_join(udp_thread, NULL);
    pthread_cancel(keepalive_thread);
    close(tcp_sock);
    return 0;
}

// Thread that listens to multicast UDP questions
void* udp_listener_thread(void* arg) {
    int udp_sock;
    struct sockaddr_in mcast_addr;
    struct ip_mreq mreq;
    char buffer[1024];

    // Create UDP socket
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MULTICAST_PORT);
    mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind to multicast port
    bind(udp_sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    // Join multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

   while (1) {
    // get question
    recvfrom(udp_sock, &msg, sizeof(msg), 0, NULL, NULL);
    msg.payload[msg.payload_len] = '\0';
    printf("\n📨 Question received:\n%s\n", msg.payload);

    // Send ACK to server via TCP
    build_message(&msg, TRV_ACK, 0, "");
    send(tcp_sock, &msg, 4 + msg.payload_len, 0);

    printf("Your answer (1/2/3/4), 30 sec timeout: ");
    fflush(stdout);


    // Set up select() on stdin with 30 sec timeout
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;

  int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (ret > 0) {
            char buffer[1024];
            fgets(buffer, sizeof(buffer), stdin);
            buffer[strcspn(buffer, "\n")] = '\0';
            build_message(&msg, TRV_ANSWER, msg.question_id, buffer);
            send(tcp_sock, &msg, 4 + msg.payload_len, 0);
        } else if (ret == 0) {
            printf("\n⏰ Time expired. No answer sent.\n");
            build_message(&msg, TRV_ANSWER, msg.question_id, "0");
            send(tcp_sock, &msg, 4 + msg.payload_len, 0);
        } else {
            perror("select failed");
        }
    }

    return NULL;
}

// Thread that sends TCP keep-alive messages every 10 seconds

void* keep_alive_thread(void* arg) {
    TrvMessage msg;
    while (1) {
        sleep(10);
        build_message(&msg, TRV_KEEPALIVE, 0, "");
        send(tcp_sock, &msg, 4 + msg.payload_len, 0);
    }
    return NULL;
}
