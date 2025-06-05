#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define TRV_QUESTION      0x01  // שאלה נשלחת מהשרת
#define TRV_ACK           0x02  // לקוח מאשר קבלת שאלה
#define TRV_ANSWER        0x03  // תשובה מהלקוח
#define TRV_KEEPALIVE     0x04  // שמירת חיבור פעיל
#define TRV_WINNER        0x05  // הכרזת מנצח
#define TRV_AUTH_CODE     0x06  // קוד אימות מהשרת
#define TRV_AUTH_REPLY    0x07  // תגובת לקוח עם קוד
#define TRV_AUTH_OK       0x08  // אימות הצליח
#define TRV_AUTH_FAIL     0x09  // אימות נכשל

#define TRV_MAX_PAYLOAD   512


typedef struct {
    char question[256];
    char options[4][128];
    int correct_index;
} TriviaQuestion;


// מבנה הודעה בפרוטוקול הטריוויה
typedef struct {
    uint8_t type;              // סוג ההודעה (TRV_*)
    uint8_t question_id;       // מזהה שאלה (רלוונטי לשאלה/תשובה)
    uint16_t payload_len;      // אורך המידע בהודעה
    char payload[TRV_MAX_PAYLOAD];  // גוף ההודעה (שאלה, תשובה, שם מנצח וכו')
} __attribute__((packed)) TrvMessage;

// פונקציה לבניית הודעה לשליחה
static inline int build_message(TrvMessage* msg, uint8_t type, uint8_t qid, const char* payload) {
    msg->type = type;
    msg->question_id = qid;
    msg->payload_len = (uint16_t)strlen(payload);
    strncpy(msg->payload, payload, TRV_MAX_PAYLOAD);
    return 4 + msg->payload_len;  // אורך כולל: header + payload
}

// פונקציה להדפסת הודעה (debug)
static inline void print_message(const TrvMessage* msg) {
    printf("== TRV Message ==\n");
    printf("Type        : 0x%02X\n", msg->type);
    printf("Question ID : %d\n", msg->question_id);
    printf("Payload Len : %d\n", msg->payload_len);
    printf("Payload     : %.*s\n", msg->payload_len, msg->payload);
}

#endif // PROTOCOL_H
