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

// Server and multicast configuration constants
#define SERVER_IP "192.3.1.1"
#define SERVER_PORT 8889
#define MULTICAST_IP "224.1.1.1"
#define MULTICAST_PORT 12345

// Utility function: Clear input buffer (flush stdin)
void clear_stdin() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

// TCP socket and server address (global, used by threads)
int tcp_sock;
struct sockaddr_in server_addr;
int auth_successful = 0; // Authentication status flag

// Thread function declarations
void* udp_listener_thread(void* arg);         // Receives questions via UDP multicast
void* keep_alive_thread(void* arg);           // Sends periodic keepalive messages over TCP
void* tcp_winner_listener_thread(void* arg);  // Listens for game result messages (e.g. winner)

// Helper function: Receive 'length' bytes from a TCP socket (handling partial reads)
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
    pthread_t udp_thread, keepalive_thread, tcp_winner_thread;

    // Prompt user to join the game
    printf("Do you want to join the game? (y/n): ");
    fgets(buffer, sizeof(buffer), stdin);
    if (buffer[0] != 'y' && buffer[0] != 'Y') {
        printf("Canceled. Exiting.\n");
        return 0;
    }

    // Get user nickname
    char nickname[64];
    printf("Enter your nickname: ");
    fgets(nickname, sizeof(nickname), stdin);
    nickname[strcspn(nickname, "\n")] = '\0';

    // --- TCP Connection Setup ---
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    memset(&(server_addr.sin_zero), 0, 8);

    // Connect to the server
    if (connect(tcp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP connection failed");
        return 1;
    }

    // --- Authentication Protocol ---

    // Wait for authentication code from the server (first message, header)
    TrvMessage msg;
    int n = recv_full(tcp_sock, &msg, 4);
    if (n <= 0) {
        printf("Server closed connection unexpectedly.\n");
        close(tcp_sock);
        return 1;
    }

    // Receive full payload if there is one
    if (msg.payload_len > 0) {
        n = recv_full(tcp_sock, msg.payload, msg.payload_len);
        if (n <= 0) {
            printf("Server closed connection during payload.\n");
            close(tcp_sock);
            return 1;
        }
    }
    msg.payload[msg.payload_len] = '\0'; // Null-terminate payload for safe printing
    printf("Server: %s\n", msg.payload);

    // If server sends an AUTH_FAIL, exit
    if (msg.type == TRV_AUTH_FAIL) {
        close(tcp_sock);
        return 1;
    }

    // User enters the authentication code
    printf("Enter code: ");
    fgets(buffer, sizeof(buffer), stdin);
    buffer[strcspn(buffer, "\n")] = '\0';

    // Prepare authentication reply message (format: code|nickname)
    char combined[128];
    snprintf(combined, sizeof(combined), "%s|%s", buffer, nickname);
    build_message(&msg, TRV_AUTH_REPLY, 0, combined);
    send(tcp_sock, &msg, 4 + msg.payload_len, 0);

    // Wait for authentication result from server
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

    // Exit if authentication failed
    if (msg.type != TRV_AUTH_OK) {
        close(tcp_sock);
        return 1;
    }

    // --- Main Game Logic Starts Here (threads for game flow) ---

    auth_successful = 1; // Mark as authenticated

    // Start UDP multicast listener (questions)
    pthread_create(&udp_thread, NULL, udp_listener_thread, NULL);

    // Start keep-alive sender (TCP)
    pthread_create(&keepalive_thread, NULL, keep_alive_thread, NULL);

    // Start winner-listener thread (TCP)
    pthread_create(&tcp_winner_thread, NULL, tcp_winner_listener_thread, NULL);

    // Main thread waits for game to finish
    pthread_join(udp_thread, NULL);          // Wait for questions to finish
    pthread_join(tcp_winner_thread, NULL);   // Wait for winner announcement

    pthread_cancel(keepalive_thread);        // Stop keepalive when game is over
    close(tcp_sock);                         // Clean up TCP socket
    return 0;
}

// --- Thread: Receives multicast UDP messages (questions from server) ---
void* udp_listener_thread(void* arg) {
    int udp_sock;
    struct sockaddr_in mcast_addr;
    struct ip_mreq mreq;
    char buffer[1024];

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MULTICAST_PORT);
    mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Allow multiple sockets to bind to the same port (for multicast)
    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bind(udp_sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    // Join the multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Listen loop: receive trivia questions and handle answers
    while (1) {
        TrvMessage msg;
        int n = recvfrom(udp_sock, &msg, sizeof(msg), 0, NULL, NULL);
        if (n <= 0) continue;

        msg.payload[msg.payload_len] = '\0';

        if (msg.type == TRV_QUESTION) {
            // Display question
            printf("\n📨 Question received:\n%s\n", msg.payload);
            fflush(stdout);

            // Immediately send ACK over TCP
            TrvMessage mACK;
            build_message(&mACK, TRV_ACK, 0, "");
            send(tcp_sock, &mACK, 4 + mACK.payload_len, 0);

            // Prompt user for answer with a timeout (30 seconds)
            printf("Your answer (1/2/3/4), 30 sec timeout: ");
            fflush(stdout);

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);
            struct timeval timeout;
            timeout.tv_sec = 30;
            timeout.tv_usec = 0;

            TrvMessage answer;
            int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
            if (ret > 0) {
                // User provided input in time
                fgets(buffer, sizeof(buffer), stdin);
                buffer[strcspn(buffer, "\n")] = '\0';
                build_message(&answer, TRV_ANSWER, msg.question_id, buffer);
                send(tcp_sock, &answer, 4 + answer.payload_len, 0);
            } else {
                // Timeout expired; send default answer ("0" = no answer)
                printf("\n⏰ Time expired. No answer sent.\n");
                build_message(&answer, TRV_ANSWER, msg.question_id, "0");
                send(tcp_sock, &answer, 4 + answer.payload_len, 0);
            }
        }
    }

    close(udp_sock);
    return NULL;
}

// --- Thread: Listens for winner/game over announcement via TCP ---
void* tcp_winner_listener_thread(void* arg) {
    TrvMessage msg;
    while (1) {
        int n = recv_full(tcp_sock, &msg, 4); // Get header
        if (n <= 0) break;

        if (msg.payload_len > 0) {
            n = recv_full(tcp_sock, msg.payload, msg.payload_len); // Get payload
            if (n <= 0) break;
        }

        msg.payload[msg.payload_len] = '\0';

        if (msg.type == TRV_WINNER) {
            printf("\n🎉 GAME OVER!\n%s\n", msg.payload);
            fflush(stdout);
            break; // Exit thread after game over
        }
    }
    return NULL;
}

// --- Thread: Periodically sends keepalive messages via TCP ---
void* keep_alive_thread(void* arg) {
    TrvMessage ka;
    while (1) {
        sleep(10); // Send every 10 seconds
        if (auth_successful) {
            build_message(&ka, TRV_KEEPALIVE, 0, "");
            send(tcp_sock, &ka, 4 + ka.payload_len, 0);
        }
    }
    return NULL;
}
