#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define PORT 8888
#define MAX_CLIENTS 10
#define GAME_LOBBY_TIME 120  // 2 דקות (120 שניות)

typedef struct {
    int socket;
    struct sockaddr_in addr;
    int verified;
    int score;
    int code_sent;  // ✅ נשמר הקוד שנשלח ללקוח
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;
int game_started = 0;

void* handle_client(void* arg);
void* game_lobby_timer();

int main() {
    srand(time(NULL));  // מזרע למספרים אקראיים
    int server_fd;
    struct sockaddr_in server_addr;
    pthread_t lobby_thread;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(server_addr.sin_zero), 0, 8);

    bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 5);

    printf("Server started on port %d. Waiting for clients...\n", PORT);
    pthread_create(&lobby_thread, NULL, game_lobby_timer, NULL);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &len);

        if (game_started || client_count >= MAX_CLIENTS) {
            printf("Rejected connection: game already started or max clients reached.\n");
            close(client_socket);
            continue;
        }

        clients[client_count].socket = client_socket;
        clients[client_count].addr = client_addr;
        clients[client_count].verified = 0;
        clients[client_count].score = 0;

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, &clients[client_count]);
        client_count++;
    }

    return 0;
}

void* handle_client(void* arg) {
    Client* client = (Client*)arg;
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));

    client->code_sent = rand() % 9000 + 1000;  // ✅ יצירת קוד 4 ספרות
    sprintf(buffer, "Enter code to verify: %d\n", client->code_sent);
    send(client->socket, buffer, strlen(buffer), 0);

    memset(buffer, 0, sizeof(buffer));
    recv(client->socket, buffer, sizeof(buffer), 0);

    if (atoi(buffer) != client->code_sent) {
        send(client->socket, "Verification failed\n", 20, 0);
        close(client->socket);
        pthread_exit(NULL);
    }

    client->verified = 1;
    send(client->socket, "Verified successfully!\n", 24, 0);
    printf("Client verified: %s:%d\n", inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));

    // כאן בעתיד נוסיף קבלת שאלות, ניקוד, וכו'
    return NULL;
}

void* game_lobby_timer() {
    printf("Lobby open for %d seconds...\n", GAME_LOBBY_TIME);
    sleep(GAME_LOBBY_TIME);
    game_started = 1;
    printf("Lobby closed. Game starting now!\n");
    // כאן אפשר לקרוא לפונקציה שמתחילה את שלב השאלות
    return NULL;
}
