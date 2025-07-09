#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

// Message type constants for the trivia protocol
#define TRV_QUESTION      0x01   // Message containing a trivia question
#define TRV_ACK           0x02   // Acknowledgment message
#define TRV_ANSWER        0x03   // Message containing a client's answer
#define TRV_KEEPALIVE     0x04   // Keepalive message to maintain connection
#define TRV_WINNER        0x05   // Message announcing the winner
#define TRV_AUTH_CODE     0x06   // Authentication code sent from server to client
#define TRV_AUTH_REPLY    0x07   // Client's reply to the authentication code
#define TRV_AUTH_OK       0x08   // Authentication successful
#define TRV_AUTH_FAIL     0x09   // Authentication failed

#define TRV_MAX_PAYLOAD   512    // Maximum payload size for message data

// Structure representing a trivia question
typedef struct {
    char question[256];       // The question text
    char options[4][128];     // Four possible answer options
    int correct_index;        // Index of the correct answer (0-3)
} TriviaQuestion;

// Structure representing a generic protocol message
typedef struct {
    uint8_t type;                   // Message type (see TRV_* defines)
    uint8_t question_id;            // ID of the question (for tracking)
    uint16_t payload_len;           // Length of the payload in bytes
    char payload[TRV_MAX_PAYLOAD];  // Payload data (question, answer, etc.)
} __attribute__((packed)) TrvMessage;

// Helper function to build a protocol message
// Returns total size of the message (header + payload)
// - msg: pointer to the TrvMessage struct to populate
// - type: message type
// - qid: question ID
// - payload: string payload (null-terminated)
static inline int build_message(TrvMessage* msg, uint8_t type, uint8_t qid, const char* payload) {
    msg->type = type;
    msg->question_id = qid;
    msg->payload_len = (uint16_t)strlen(payload);
    strncpy(msg->payload, payload, TRV_MAX_PAYLOAD);
    return 4 + msg->payload_len; // Header is 4 bytes, plus payload
}

// Utility function to print the contents of a protocol message (for debugging)
static inline void print_message(const TrvMessage* msg) {
    printf("== TRV Message ==\n");
    printf("Type        : 0x%02X\n", msg->type);
    printf("Question ID : %d\n", msg->question_id);
    printf("Payload Len : %d\n", msg->payload_len);
    printf("Payload     : %.*s\n", msg->payload_len, msg->payload);
}

#endif // PROTOCOL_H
