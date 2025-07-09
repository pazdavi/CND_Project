// #define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>
#include "protocol.h"

// ---- Constants for server and game configuration ----
#define PORT 8889
#define MAX_CLIENTS 10
#define GAME_LOBBY_TIME 30
#define MULTICAST_IP "224.1.1.1"
#define MULTICAST_PORT 12345
#define ANSWER_TIMEOUT 30
#define KEEPALIVE_TIMEOUT 10.2

// ---- Client information structure ----
typedef struct {
    int socket;                     // TCP socket for communication with client
    struct sockaddr_in addr;        // Client address
    int verified;                   // 1 if authenticated, 0 otherwise
    int score;                      // Trivia score
    int auth_code;                  // Auth code to verify client
    time_t last_keepalive;          // Last keepalive timestamp
    char nickname[32];              // Player's nickname
} Client;

Client clients[MAX_CLIENTS];        // Array of connected clients
int client_count = 0;               // Current number of clients
int game_started = 0;               // 1 if game has started, 0 if still in lobby

// ---- Sample trivia questions ----
TriviaQuestion questions[6] = {
    {"Which course is the best in CSE?", {"Computer Networks Design", "Intro to Electrical Engineering", "Data Structures", "Sadna Akademit"}, 0},
    {"What is Paz's Dog's name?", {"Chili", "Nala", "Lucy", "Mitzi"}, 0},
    {"Who is Ron Zimerman's favorite singer?", {"Shiri Maimon", "Mergui", "Noa Kirel", "Anna Zak"}, 2},
    {"In an M/M/1 queue, what does the “1” represent?", {"One arrival process", "One service channel (server)", "One customer in the system", "One time unit per service"}, 1},
    {"Which cat is hairless?", {"Maine Coon", "Bengal", "Siamese", "Sphynx"}, 3},
    {"When is Efi Korenfeld's birthday?", {"September 29th", "April 14th", "July 22nd", "May 14th"}, 1}
};

// ---- Function declarations ----
void* handle_client(void* arg);
void* game_lobby_timer(void* arg);
void* keepalive_checker(void* arg);
void start_game();
void announce_winner_and_close();
void send_multicast_message(TrvMessage* msg);

// ---- Helper: Receive exactly len bytes from a socket ----
int recv_full(int sock, void* buf, int len) {
    int received = 0;
    while (received < len) {
        int n = recv(sock, (char*)buf + received, len - received, 0);
        if (n <= 0) return n;
        received += n;
    }
    return received;
}

// ---- Main server function ----
int main() {
    srand(time(NULL));  // Initialize random seed (for auth codes)
    int server_fd;
    struct sockaddr_in server_addr;
    pthread_t lobby_thread, keep_thread;

    // --- Create and set up the TCP server socket ---
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 5);

    printf("Server running on port %d. Waiting for clients...\n", PORT);

    // --- Start lobby and keepalive threads ---
    pthread_create(&lobby_thread, NULL, game_lobby_timer, NULL);
    pthread_create(&keep_thread, NULL, keepalive_checker, NULL);

    // --- Accept client connections while lobby is open ---
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &len);

        // If game started or lobby full, reject new clients
        if (game_started || client_count >= MAX_CLIENTS) {
            TrvMessage reject_msg;
            build_message(&reject_msg, TRV_AUTH_FAIL, 0, "Game already started or lobby full.");
            send(client_sock, &reject_msg, 4 + reject_msg.payload_len, 0);
            close(client_sock);
            continue;
        }

        // Initialize client struct
        clients[client_count].socket = client_sock;
        clients[client_count].addr = client_addr;
        clients[client_count].verified = 0;
        clients[client_count].score = 0;
        strcpy(clients[client_count].nickname, "(unknown)");

        // Create a thread to handle the client
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, &clients[client_count]);
        client_count++;
    }
    return 0;
}

// ---- Per-client thread: handle authentication and answers ----
void* handle_client(void* arg) {
    Client* client = (Client*)arg;
    TrvMessage msg;

    // --- Send random authentication code to client ---
    client->auth_code = rand() % 9000 + 1000;  // Random 4-digit code
    char code_str[32];
    snprintf(code_str, sizeof(code_str), "%d", client->auth_code);
    build_message(&msg, TRV_AUTH_CODE, 0, code_str);
    send(client->socket, &msg, 4 + msg.payload_len, 0);

    // --- Receive authentication reply (code|nickname) from client ---
    int n = recv_full(client->socket, &msg, 4);
    if (n <= 0) {
        close(client->socket);
        pthread_exit(NULL);
    }
    if (msg.payload_len > 0) {
        n = recv_full(client->socket, msg.payload, msg.payload_len);
        if (n <= 0) {
            close(client->socket);
            pthread_exit(NULL);
        }
    }
    msg.payload[msg.payload_len] = '\0';

    // --- Check if game started during authentication ---
    if (game_started) {
        build_message(&msg, TRV_AUTH_FAIL, 0, "Game already started.");
        send(client->socket, &msg, 4 + msg.payload_len, 0);
        close(client->socket);
        pthread_exit(NULL);
    }

    // --- Verify token and nickname ---
    char* token = strtok(msg.payload, "|");
    char* nickname = strtok(NULL, "|");
    if (!token || !nickname || atoi(token) != client->auth_code) {
        build_message(&msg, TRV_AUTH_FAIL, 0, "Invalid code or nickname.");
        send(client->socket, &msg, 4 + msg.payload_len, 0);
        close(client->socket);
        pthread_exit(NULL);
    }

    client->verified = 1;
    strncpy(client->nickname, nickname, sizeof(client->nickname));
    client->nickname[sizeof(client->nickname) - 1] = '\0';
    client->last_keepalive = time(NULL);

    build_message(&msg, TRV_AUTH_OK, 0, "Welcome to the trivia game!");
    send(client->socket, &msg, 4 + msg.payload_len, 0);

    printf("✅ %s connected and verified.\n", client->nickname);

    // --- Main per-client message loop: keepalive & answer handling ---
    while (1) {
        n = recv_full(client->socket, &msg, 4);
        if (n <= 0) break;
        if (msg.payload_len > 0) {
            n = recv_full(client->socket, msg.payload, msg.payload_len);
            if (n <= 0) break;
        }
        msg.payload[msg.payload_len] = '\0';

        if (msg.type == TRV_KEEPALIVE) {
            client->last_keepalive = time(NULL);
            printf("🔄 KEEPALIVE received from %s (%s)\n",
                   client->nickname, inet_ntoa(client->addr.sin_addr));
        } else if (msg.type == TRV_ANSWER) {
            int qid = msg.question_id;
            int ans = atoi(msg.payload);
            if (qid >= 0 && qid < 6) {
                if (ans == questions[qid].correct_index + 1) {
                    client->score++;
                    printf("✅ %s answered question %d correctly. Score: %d\n",
                           client->nickname, qid + 1, client->score);
                } else {
                    printf("❌ %s answered question %d incorrectly.\n",
                           client->nickname, qid + 1);
                }
            }
        }
    }

    close(client->socket);
    pthread_exit(NULL);
}

// ---- Thread: handles lobby timer, then starts the game ----
void* game_lobby_timer(void* arg) {
    printf("Lobby open for %d seconds...\n", GAME_LOBBY_TIME);
    sleep(GAME_LOBBY_TIME);
    game_started = 1;
    printf("Lobby closed. Starting game!\n");
    start_game();
    return NULL;
}

// ---- Main function to multicast questions and collect answers ----
void start_game() {
    // --- Prepare UDP socket for multicast ---
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct in_addr localInterface;
    localInterface.s_addr = inet_addr("192.3.1.1"); // Interface for multicast
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, (char *)&localInterface, sizeof(localInterface));
    unsigned char ttl = 10;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    mcast_addr.sin_port = htons(MULTICAST_PORT);

    // Send dummy data to help clients join group early
    char dummy_data[1] = {0};
    sendto(sockfd, dummy_data, sizeof(dummy_data), 0,
           (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    printf("⌛ Waiting 2 seconds before sending questions...\n");
    sleep(2);

    // --- Send each question as a multicast UDP packet ---
    TrvMessage question;
    for (int i = 0; i < 6; i++) {
        char q_text[512];
        snprintf(q_text, sizeof(q_text), "Question #%d:\n%s\n1. %s\n2. %s\n3. %s\n4. %s",
                 i + 1,
                 questions[i].question,
                 questions[i].options[0],
                 questions[i].options[1],
                 questions[i].options[2],
                 questions[i].options[3]);

        build_message(&question, TRV_QUESTION, i, q_text);
        int res = sendto(sockfd, &question, 4 + question.payload_len, 0,
                         (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
        if (res < 0) perror("sendto failed");

        printf("📨 Sent question %d. Waiting for answers...\n", i + 1);
        sleep(ANSWER_TIMEOUT); // Wait before next question
    }

    close(sockfd);
    announce_winner_and_close();
}

// ---- Announce winner to all clients and close server ----
void announce_winner_and_close() {
    int highest = -1;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].verified && clients[i].score > highest) {
            highest = clients[i].score;
        }
    }

    char message[1024];
    strcpy(message, "\n\n=== Final Scores ===\n");
    for (int i = 0; i < client_count; i++) {
        if (clients[i].verified) {
            char line[128];
            snprintf(line, sizeof(line), "%s: %d\n", clients[i].nickname, clients[i].score);
            strcat(message, line);
        }
    }

    strcat(message, "\n");
    int winners = 0;
    char winner_name[32];
    for (int i = 0; i < client_count; i++) {
        if (clients[i].verified && clients[i].score == highest) {
            winners++;
            strcpy(winner_name, clients[i].nickname);
        }
    }

    if (winners == 0) {
        strcat(message, "No one answered any questions correctly.\n");
    } else if (winners == 1) {
        strcat(message, "\n🏆 Winner: ");
        strcat(message, winner_name);
        strcat(message, "!\n");
    } else {
        strcat(message, "\n⚔️ It's a tie between multiple players!\n");
    }

    TrvMessage winmsg;
    build_message(&winmsg, TRV_WINNER, 0, message);

    // Send to all clients (TCP)
    for (int i = 0; i < client_count; i++) {
        if (clients[i].verified) {
            send(clients[i].socket, &winmsg, 4 + winmsg.payload_len, 0);
            close(clients[i].socket);
        }
    }

    // Also announce result via multicast
    send_multicast_message(&winmsg);

    printf("%s", message);
    printf("\nGame over. Shutting down server.\n");
    exit(0);
}

// ---- Helper: send a message via UDP multicast ----
void send_multicast_message(TrvMessage* msg) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct in_addr localInterface;
    localInterface.s_addr = inet_addr("192.3.1.1");
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, (char *)&localInterface, sizeof(localInterface));

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    mcast_addr.sin_port = htons(MULTICAST_PORT);

    sendto(sockfd, msg, 4 + msg->payload_len, 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    close(sockfd);
}

// ---- Thread: checks client keepalives, removes dead clients ----
void* keepalive_checker(void* arg) {
    while (1) {
        sleep(5); // Check every 5 seconds
        time_t now = time(NULL);
        for (int i = 0; i < client_count; i++) {
            if (clients[i].verified &&
                difftime(now, clients[i].last_keepalive) > KEEPALIVE_TIMEOUT) {
                printf("⚠️  %s timed out.\n", clients[i].nickname);
                close(clients[i].socket);
                clients[i].verified = 0;
            }
        }
    }
    return NULL;
}
