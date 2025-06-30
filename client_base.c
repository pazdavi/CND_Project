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

#define SERVER_IP "192.3.2.1"
#define SERVER_PORT 8889
#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 12345

int tcp_sock;
struct sockaddr_in server_addr;

// Thread function declarations
void* udp_listener_thread(void* arg);
void* keep_alive_thread(void* arg);

// Helper to read exactly 'length' bytes from TCP socket
int recv_full(int sock, void* buf, int length) {
    int received = 0;
    while (received < length) {
        int n = recv(sock, (char*)buf + received, length - received, 0);
        if (n <= 0) return n;
        received += n;
    }
    return received;
}

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

    // Read header
    int n = recv_full(tcp_sock, &msg, 4);
    if (n <= 0) {
        printf("Server closed connection unexpectedly.\n");
        close(tcp_sock);
        return 1;
    }
    // Read payload
    if (msg.payload_len > 0) {
        n = recv_full(tcp_sock, msg.payload, msg.payload_len);
        if (n <= 0) {
            printf("Server closed connection during payload.\n");
            close(tcp_sock);
            return 1;
        }
    }
    msg.payload[msg.payload_len] = '\0';
    printf("Server: %s\n", msg.payload);

    if (msg.type == TRV_AUTH_FAIL) {
        printf("Server rejected connection: %s\n", msg.payload);
        close(tcp_sock);
        return 1;
    }

    // User inputs the code
    printf("Enter code: ");
    // Flush stdin
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
    fgets(buffer, sizeof(buffer), stdin);
    buffer[strcspn(buffer, "\n")] = '\0';

    build_message(&msg, TRV_AUTH_REPLY, 0, buffer);
    send(tcp_sock, &msg, 4 + msg.payload_len, 0);

    // Receive verification result
    n = recv_full(tcp_sock, &msg, 4);
    if (n <= 0) {
        printf("Server closed connection during verification.\n");
        close(tcp_sock);
        return 1;
    }
    if (msg.payload_len > 0) {
        n = recv_full(tcp_sock, msg.payload, msg.payload_len);
        if (n <= 0) {
            printf("Server closed connection during payload.\n");
            close(tcp_sock);
            return 1;
        }
    }
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

    // Enable reuse
    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    // Bind to multicast port
    bind(udp_sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    // Join multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    while (1) {
        // get question
        TrvMessage question;
        recvfrom(udp_sock, &question, sizeof(question), 0, NULL, NULL);
        question.payload[question.payload_len] = '\0';
        printf("\n📨 Question received:\n%s\n", question.payload);

        // Send ACK to server via TCP
        TrvMessage mACK;
        build_message(&mACK, TRV_ACK, 0, "");
        send(tcp_sock, &mACK, 4 + mACK.payload_len, 0);

        printf("Your answer (1/2/3/4), 30 sec timeout: ");
        fflush(stdout);

        // Set up select() on stdin with 30 sec timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        struct timeval timeout;
        timeout.tv_sec = 30;
        timeout.tv_usec = 0;

        TrvMessage answer;
        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (ret > 0) {
            fgets(buffer, sizeof(buffer), stdin);
            buffer[strcspn(buffer, "\n")] = '\0';
            build_message(&answer, TRV_ANSWER, question.question_id, buffer);
            send(tcp_sock, &answer, 4 + answer.payload_len, 0);
        } else if (ret == 0) {
            printf("\n⏰ Time expired. No answer sent.\n");
            build_message(&answer, TRV_ANSWER, question.question_id, "0");
            send(tcp_sock, &answer, 4 + answer.payload_len, 0);
        } else {
            perror("select failed");
        }
    }

    return NULL;
}

// Thread that sends TCP keep-alive messages every 10 seconds
void* keep_alive_thread(void* arg) {
    TrvMessage ka;
    while (1) {
        sleep(10);
        build_message(&ka, TRV_KEEPALIVE, 0, "");
        send(tcp_sock, &ka, 4 + ka.payload_len, 0);
    }
    return NULL;
}
