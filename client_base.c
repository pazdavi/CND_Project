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

#define SERVER_IP "192.3.1.1"
#define SERVER_PORT 8889
#define MULTICAST_IP "224.1.1.1"
#define MULTICAST_PORT 12345

// function to clear buffer when not answering questions 
void clear_stdin() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

int tcp_sock;
struct sockaddr_in server_addr;
int auth_successful = 0;

void* udp_listener_thread(void* arg);
void* keep_alive_thread(void* arg);
void* tcp_winner_listener_thread(void* arg);

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

    printf("Do you want to join the game? (y/n): ");
    fgets(buffer, sizeof(buffer), stdin);
    if (buffer[0] != 'y' && buffer[0] != 'Y') {
        printf("Canceled. Exiting.\n");
        return 0;
    }

    char nickname[64];
    printf("Enter your nickname: ");
    fgets(nickname, sizeof(nickname), stdin);
    nickname[strcspn(nickname, "\n")] = '\0';

    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    memset(&(server_addr.sin_zero), 0, 8);

    if (connect(tcp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP connection failed");
        return 1;
    }

    TrvMessage msg;
    int n = recv_full(tcp_sock, &msg, 4);
    if (n <= 0) {
        printf("Server closed connection unexpectedly.\n");
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

    if (msg.type == TRV_AUTH_FAIL) {
        close(tcp_sock);
        return 1;
    }

    printf("Enter code: ");
    fgets(buffer, sizeof(buffer), stdin);
    buffer[strcspn(buffer, "\n")] = '\0';

    char combined[128];
    snprintf(combined, sizeof(combined), "%s|%s", buffer, nickname);
    build_message(&msg, TRV_AUTH_REPLY, 0, combined);
    send(tcp_sock, &msg, 4 + msg.payload_len, 0);

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
        close(tcp_sock);
        return 1;
    }

    auth_successful = 1;
    pthread_create(&udp_thread, NULL, udp_listener_thread, NULL);
    pthread_create(&keepalive_thread, NULL, keep_alive_thread, NULL);
    pthread_create(&tcp_winner_thread, NULL, tcp_winner_listener_thread, NULL);

    pthread_join(udp_thread, NULL);
    pthread_join(tcp_winner_thread, NULL);
    pthread_cancel(keepalive_thread);
    close(tcp_sock);
    return 0;
}

void* udp_listener_thread(void* arg) {
    int udp_sock;
    struct sockaddr_in mcast_addr;
    struct ip_mreq mreq;
    char buffer[1024];

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MULTICAST_PORT);
    mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bind(udp_sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    while (1) {
        TrvMessage msg;
        int n = recvfrom(udp_sock, &msg, sizeof(msg), 0, NULL, NULL);
        if (n <= 0) continue;

        msg.payload[msg.payload_len] = '\0';

        if (msg.type == TRV_QUESTION) {
            printf("\n📨 Question received:\n%s\n", msg.payload);
            fflush(stdout);

            TrvMessage mACK;
            build_message(&mACK, TRV_ACK, 0, "");
            send(tcp_sock, &mACK, 4 + mACK.payload_len, 0);

            printf("Your answer (1/2/3/4), 30 sec timeout: ");
            fflush(stdout);
			
			// clear input buffer before answer
			//clear_stdin();

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
                build_message(&answer, TRV_ANSWER, msg.question_id, buffer);
				
				//clear_stdin();
				
                send(tcp_sock, &answer, 4 + answer.payload_len, 0);
            } else {
                printf("\n⏰ Time expired. No answer sent.\n");
                build_message(&answer, TRV_ANSWER, msg.question_id, "0");
                send(tcp_sock, &answer, 4 + answer.payload_len, 0);
            }
        }
    }

    close(udp_sock);
    return NULL;
}

void* tcp_winner_listener_thread(void* arg) {
    TrvMessage msg;
    while (1) {
        int n = recv_full(tcp_sock, &msg, 4);
        if (n <= 0) break;

        if (msg.payload_len > 0) {
            n = recv_full(tcp_sock, msg.payload, msg.payload_len);
            if (n <= 0) break;
        }

        msg.payload[msg.payload_len] = '\0';

        if (msg.type == TRV_WINNER) {
            printf("\n🎉 GAME OVER!\n%s\n", msg.payload);
            fflush(stdout);
            break;
        }
    }
    return NULL;
}

void* keep_alive_thread(void* arg) {
    TrvMessage ka;
    while (1) {
        sleep(10);
        if (auth_successful) {
            build_message(&ka, TRV_KEEPALIVE, 0, "");
            send(tcp_sock, &ka, 4 + ka.payload_len, 0);
        }
    }
    return NULL;
}
