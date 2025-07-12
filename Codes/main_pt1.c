// ==== FreeRTOS and standard includes ====
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ==== Configuration macros ====
#define MIN_PACKET_SIZE 500            // Minimum packet size in bytes
#define MAX_PACKET_SIZE 1500           // Maximum packet size in bytes
#define ACK_LENGTH 40                  // Acknowledgment packet size
#define LINK_CAPACITY 100000           // Link capacity in bits/sec
#define PROPAGATION_DELAY_MS 5         // Fixed propagation delay in ms
#define MAX_RETRIES 4                  // Max retransmissions per packet
#define MIN_INTERVAL_MS 100            // Min packet generation interval
#define MAX_INTERVAL_MS 200            // Max packet generation interval
#define QUEUE_LENGTH 20                // Queue capacity
#define RETRY_LIST_SIZE 100            // Buffer for tracking in-flight packets

#define ACK_TIMEOUT_MS 200             // Timeout before retrying a packet
#define P_drop 4                       // Drop probability (%) of data packets

// ==== Packet and ACK structure definitions ====
typedef struct {
    uint8_t sender_id;                // ID of the sender (1 or 2)
    uint8_t destination;             // Receiver ID (3 or 4)
    uint16_t length;                 // Total length of packet in bytes
    uint32_t sequence_num;           // Per-destination sequence number
} PacketHeader;

typedef struct {
    PacketHeader header;
    uint8_t data[];                  // Flexible payload buffer
} Packet;

typedef struct {
    uint8_t source;                  // Receiver ID sending ACK
    uint8_t destination;             // Sender ID to receive ACK
    uint32_t sequence_num;           // Sequence number being acknowledged
} AckPacket;

// Tracks packets awaiting ACKs
typedef struct {
    Packet *data_packet;             // Original packet waiting for ACK
    TickType_t timeout_tick;         // Time after which to retry
    uint8_t retries;                 // Number of retries so far
    BaseType_t active;               // Is this entry active?
    uint8_t receiver_id;             // Receiver node that should ACK
} PendingAck;

// ==== Queues for communication ====
QueueHandle_t sender1Queue, sender2Queue;
QueueHandle_t xSwitchQueue, xReceiver3Queue, xReceiver4Queue, xAckQueue;

// ==== Timers for packet generation ====
TimerHandle_t sender1Timer, sender2Timer;

// ==== Sequence number counters ====
uint32_t seq1_3 = 0, seq1_4 = 0, seq2_3 = 0, seq2_4 = 0;

// ==== Counters for stats ====
volatile uint32_t packetsGenerated[2] = {0};
volatile uint32_t packetsReceived[2] = {0};
volatile uint32_t packetsDropped = 0;
volatile uint32_t packetsFailedToDeliver = 0;
volatile int simulation_time = 0;

// ==== Retry tracking buffer ====
PendingAck retryBuffer[RETRY_LIST_SIZE];

// ==== Utility functions ====
uint8_t getRandomDest() {
    return (rand() % 2 == 0) ? 3 : 4;
}

uint16_t getRandomLen() {
    return MIN_PACKET_SIZE + rand() % (MAX_PACKET_SIZE - MIN_PACKET_SIZE + 1);
}

TickType_t getRandomInterval() {
    return pdMS_TO_TICKS(MIN_INTERVAL_MS + rand() % (MAX_INTERVAL_MS - MIN_INTERVAL_MS + 1));
}

// ==== Retry Management ====
void addToRetryList(Packet *pkt, uint8_t receiver_id) {
    for (int i = 0; i < RETRY_LIST_SIZE; i++) {
        if (!retryBuffer[i].active) {
            retryBuffer[i].data_packet = pkt;
            retryBuffer[i].timeout_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ACK_TIMEOUT_MS);
            retryBuffer[i].retries = 0;
            retryBuffer[i].active = pdTRUE;
            retryBuffer[i].receiver_id = receiver_id;
            return;
        }
    }
}

// Retry logic
void resendPacket(PendingAck *entry) {
    if (entry->retries < MAX_RETRIES) {
        xQueueSend(xSwitchQueue, &entry->data_packet, 0);
        entry->timeout_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ACK_TIMEOUT_MS);
        entry->retries++;
    } else {
        free(entry->data_packet);
        entry->active = pdFALSE;
        packetsFailedToDeliver++;
    }
}

// ==== Packet Generation Timer Callback ====
void senderCallback(TimerHandle_t xTimer) {
    uint8_t sender_id = (uint32_t)pvTimerGetTimerID(xTimer);  // Get sender ID from timer
    QueueHandle_t queue = (sender_id == 1) ? sender1Queue : sender2Queue;

    // Use proper sequence counter based on destination
    uint32_t *seq3 = (sender_id == 1) ? &seq1_3 : &seq2_3;
    uint32_t *seq4 = (sender_id == 1) ? &seq1_4 : &seq2_4;
    uint32_t *counter = &packetsGenerated[sender_id - 1];

    uint16_t len = getRandomLen();  // Random packet size
    Packet *pkt = malloc(len);
    if (!pkt) return;

    pkt->header.sender_id = sender_id;
    pkt->header.destination = getRandomDest();
    pkt->header.sequence_num = (pkt->header.destination == 3) ? (*seq3)++ : (*seq4)++;
    pkt->header.length = len;
    memset(pkt->data, (sender_id == 1) ? 'A' : 'B', len - sizeof(PacketHeader));

    xQueueSend(queue, &pkt, portMAX_DELAY);
    (*counter)++;
}

// ==== Sender Tasks ====
void vSenderTask(void *pvParams) {
    QueueHandle_t myQueue = (QueueHandle_t)pvParams;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(myQueue, &pkt, portMAX_DELAY) == pdPASS) {
            xQueueSend(xSwitchQueue, &pkt, portMAX_DELAY);  // Forward to switch
        }
    }
}

// ==== Receiver Tasks ====
void vReceiverTask(void *pvParams) {
    QueueHandle_t rxQ = (QueueHandle_t)pvParams;
    uint8_t myId = (rxQ == xReceiver3Queue) ? 3 : 4;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(rxQ, &pkt, portMAX_DELAY) == pdPASS) {
            // On receive, send ACK
            AckPacket *ack = malloc(sizeof(AckPacket));
            if (ack) {
                ack->source = myId;
                ack->destination = pkt->header.sender_id;
                ack->sequence_num = pkt->header.sequence_num;
                xQueueSend(xAckQueue, &ack, portMAX_DELAY);
            }
        }
    }
}

// ==== Switch Task ====
void vSwitchTask(void *pvParams) {
    void *msg;
    while (1) {
        TickType_t now = xTaskGetTickCount();

        // Check for timeouts
        for (int i = 0; i < RETRY_LIST_SIZE; i++) {
            if (retryBuffer[i].active && now >= retryBuffer[i].timeout_tick) {
                resendPacket(&retryBuffer[i]);
            }
        }

        // Process incoming data packet
        if (xQueueReceive(xSwitchQueue, &msg, pdMS_TO_TICKS(10)) == pdPASS) {
            Packet *pkt = (Packet *)msg;

            if ((rand() % 100) < P_drop) {
                free(pkt);
                packetsDropped++;
                continue;
            }

            // Apply delay: propagation + transmission
            TickType_t delay = pdMS_TO_TICKS(PROPAGATION_DELAY_MS) +
                pdMS_TO_TICKS((pkt->header.length * 8 * 1000) / LINK_CAPACITY);
            vTaskDelay(delay);

            QueueHandle_t destQ = (pkt->header.destination == 3) ? xReceiver3Queue : xReceiver4Queue;
            xQueueSend(destQ, &pkt, portMAX_DELAY);
            addToRetryList(pkt, pkt->header.destination);
        }

        // Process ACKs
        AckPacket *ack;
        if (xQueueReceive(xAckQueue, &ack, 0) == pdPASS) {
            TickType_t ackDelay = pdMS_TO_TICKS(PROPAGATION_DELAY_MS) +
                pdMS_TO_TICKS((ACK_LENGTH * 8 * 1000) / LINK_CAPACITY);
            vTaskDelay(ackDelay);

            if ((rand() % 100) < 1) {
                free(ack);
                continue;
            }

            now = xTaskGetTickCount();
            for (int i = 0; i < RETRY_LIST_SIZE; i++) {
                if (retryBuffer[i].active &&
                    retryBuffer[i].data_packet->header.sender_id == ack->destination &&
                    retryBuffer[i].data_packet->header.sequence_num == ack->sequence_num) {

                    if (now <= retryBuffer[i].timeout_tick) {
                        uint8_t recv = retryBuffer[i].receiver_id;
                        packetsReceived[recv - 3]++;
                    }

                    free(retryBuffer[i].data_packet);
                    retryBuffer[i].active = pdFALSE;
                    break;
                }
            }
            free(ack);
        }
    }
}

// ==== Statistics Output Task ====
void vStatsTask(void *pvParameters) {
    const double avgPacketSize = (MIN_PACKET_SIZE + MAX_PACKET_SIZE) / 2.0;
    while (1) {
        int suspended = packetsGenerated[0] + packetsGenerated[1]
                      - packetsReceived[0] - packetsReceived[1]
                      - packetsDropped - packetsFailedToDeliver;

        uint32_t totalPacketsReceived = packetsReceived[0] + packetsReceived[1];
        uint32_t totalBytesReceived = (uint32_t)(totalPacketsReceived * avgPacketSize);

        printf("\n======== Communication Stats ========\n");
        printf("Tout: %d ms\n", ACK_TIMEOUT_MS);
        printf("Pdrop: %d \n", P_drop);
        printf("Time: %d s\n", simulation_time);
        printf("Sender 1 - Packets Generated: %lu\n", packetsGenerated[0]);
        printf("Sender 2 - Packets Generated: %lu\n", packetsGenerated[1]);
        printf("Receiver 3 - Packets Received: %lu\n", packetsReceived[0]);
        printf("Receiver 4 - Packets Received: %lu\n", packetsReceived[1]);
        printf("Dropped (Random P_drop): %lu\n", packetsDropped);
        printf("Failed After Retries: %lu\n", packetsFailedToDeliver);
        printf("Suspended: %d\n", suspended);

        if (simulation_time > 0) {
            uint32_t throughput_Bps = (uint32_t)totalBytesReceived / simulation_time;
            printf("Throughput: %lu bytes/sec\n", throughput_Bps);
        } else {
            printf("Throughput: N/A (waiting for time to pass)\n");
        }

        printf("=====================================\n");

        fflush(stdout);
        simulation_time += 2;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ==== Required FreeRTOS Hook Functions ====
void vApplicationMallocFailedHook(void) { while (1); }
void vApplicationTickHook(void) {}
void vApplicationIdleHook(void) {}
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask; (void)pcTaskName; while (1);
}

// ==== Main Function ====
int main(void) {
    srand((unsigned int)time(NULL));

    // Create communication queues
    sender1Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    sender2Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    xSwitchQueue = xQueueCreate(QUEUE_LENGTH, sizeof(void *));
    xReceiver3Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    xReceiver4Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    xAckQueue = xQueueCreate(QUEUE_LENGTH, sizeof(AckPacket *));

    // Start timers to generate packets
    sender1Timer = xTimerCreate("S1Gen", getRandomInterval(), pdTRUE, (void *)1, senderCallback);
    sender2Timer = xTimerCreate("S2Gen", getRandomInterval(), pdTRUE, (void *)2, senderCallback);
    xTimerStart(sender1Timer, 0);
    xTimerStart(sender2Timer, 0);

    // Create tasks
    xTaskCreate(vSenderTask, "Sender1", 2000, sender1Queue, 3, NULL);
    xTaskCreate(vSenderTask, "Sender2", 2000, sender2Queue, 3, NULL);
    xTaskCreate(vSwitchTask, "Switch", 2000, NULL, 3, NULL);
    xTaskCreate(vReceiverTask, "Receiver3", 2000, xReceiver3Queue, 2, NULL);
    xTaskCreate(vReceiverTask, "Receiver4", 2000, xReceiver4Queue, 2, NULL);
    xTaskCreate(vStatsTask, "Stats", 2000, NULL, 1, NULL);

    vTaskStartScheduler();  // Start RTOS scheduler
    while (1);
    return 0;
}
