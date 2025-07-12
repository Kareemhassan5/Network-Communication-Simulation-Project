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
#define WINDOW_SIZE 4  // Maximum number of unacknowledged packets in flight
#define MIN_PACKET_SIZE 500
#define MAX_PACKET_SIZE 1500
#define ACK_LENGTH 40
#define LINK_CAPACITY 100000  // Bits per second
#define PROPAGATION_DELAY_MS 5
#define MAX_RETRIES 4  // Max number of times to retry a packet
#define MIN_INTERVAL_MS 100
#define MAX_INTERVAL_MS 200
#define QUEUE_LENGTH 20
#define RETRY_LIST_SIZE 100
#define ACK_TIMEOUT_MS 200
#define P_drop 4  // % chance a packet is dropped randomly

// ==== Window management structure per sender ====
typedef struct {
    uint32_t base_seq_3;  // Base sequence number for packets to receiver 3
    uint32_t next_seq_3;  // Next sequence number for receiver 3
    uint32_t base_seq_4;  // Base sequence number for packets to receiver 4
    uint32_t next_seq_4;  // Next sequence number for receiver 4
} SenderWindow;

// Initialize window state for both senders
SenderWindow sender1Window = {0, 0, 0, 0};
SenderWindow sender2Window = {0, 0, 0, 0};

// ==== Packet and ACK structure definitions ====
typedef struct {
    uint8_t sender_id;      // Who sent it
    uint8_t destination;    // Who it is for
    uint16_t length;        // Total size of the packet
    uint32_t sequence_num;  // Sequence number of the packet
} PacketHeader;

typedef struct {
    PacketHeader header;    // Metadata for packet
    uint8_t data[];         // Variable-length payload
} Packet;

typedef struct {
    uint8_t source;         // Receiver sending the ACK
    uint8_t destination;    // Sender to whom ACK is sent
    uint32_t sequence_num;  // Which packet is being acknowledged
} AckPacket;

// Track retry-related info for each in-flight packet
typedef struct {
    Packet *data_packet;        // Original packet to resend
    TickType_t timeout_tick;    // When to retry it
    uint8_t retries;            // How many retries so far
    BaseType_t active;          // Is this retry entry in use?
    uint8_t receiver_id;        // Receiver expected to ACK
} PendingAck;

// ==== Queues and timers ====
QueueHandle_t sender1Queue, sender2Queue;
QueueHandle_t xSwitchQueue, xReceiver3Queue, xReceiver4Queue, xAckQueue;
TimerHandle_t sender1Timer, sender2Timer;

// ==== Global state ====
uint32_t seq1_3 = 0, seq1_4 = 0, seq2_3 = 0, seq2_4 = 0;
volatile uint32_t packetsGenerated[2] = {0};
volatile uint32_t packetsReceived[2] = {0};
volatile uint32_t packetsDropped = 0;
volatile uint32_t packetsFailedToDeliver = 0;
volatile int simulation_time = 0;
PendingAck retryBuffer[RETRY_LIST_SIZE];

// ==== Utility functions ====
// Randomly select destination: 3 or 4
uint8_t getRandomDest() {
    return (rand() % 2 == 0) ? 3 : 4;
}

// Random packet length in range
uint16_t getRandomLen() {
    return MIN_PACKET_SIZE + rand() % (MAX_PACKET_SIZE - MIN_PACKET_SIZE + 1);
}

// Random interval for packet generation
TickType_t getRandomInterval() {
    return pdMS_TO_TICKS(MIN_INTERVAL_MS + rand() % (MAX_INTERVAL_MS - MIN_INTERVAL_MS + 1));
}

// ==== Retry management ====
// Add a packet to the retry list so it can be monitored
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

// If timeout occurs, resend the packet unless retries exceeded
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

// ==== Packet generation callback ====
void senderCallback(TimerHandle_t xTimer) {
    // Determine which sender triggered the timer
    uint8_t sender_id = (uint32_t)pvTimerGetTimerID(xTimer);
    QueueHandle_t queue = (sender_id == 1) ? sender1Queue : sender2Queue;
    SenderWindow *window = (sender_id == 1) ? &sender1Window : &sender2Window;

    uint16_t len = getRandomLen();
    uint8_t dest = getRandomDest();
    uint32_t *base = (dest == 3) ? &window->base_seq_3 : &window->base_seq_4;
    uint32_t *next = (dest == 3) ? &window->next_seq_3 : &window->next_seq_4;

    // If window is full, do not generate a new packet
    if ((*next - *base) >= WINDOW_SIZE) return;

    // Create and fill the packet
    Packet *pkt = malloc(len);
    if (!pkt) return;

    pkt->header.sender_id = sender_id;
    pkt->header.destination = dest;
    pkt->header.sequence_num = (*next)++;
    pkt->header.length = len;
    memset(pkt->data, (sender_id == 1) ? 'A' : 'B', len - sizeof(PacketHeader));

    // Send packet to sender task queue
    xQueueSend(queue, &pkt, portMAX_DELAY);
    packetsGenerated[sender_id - 1]++;
}

// ==== Sender task ====
// Fetch packets from its queue and forward them to the switch
void vSenderTask(void *pvParams) {
    QueueHandle_t myQueue = (QueueHandle_t)pvParams;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(myQueue, &pkt, portMAX_DELAY) == pdPASS) {
            xQueueSend(xSwitchQueue, &pkt, portMAX_DELAY);
        }
    }
}

// ==== Receiver task ====
// Wait for packets, then respond with ACKs
void vReceiverTask(void *pvParams) {
    QueueHandle_t rxQ = (QueueHandle_t)pvParams;
    uint8_t myId = (rxQ == xReceiver3Queue) ? 3 : 4;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(rxQ, &pkt, portMAX_DELAY) == pdPASS) {
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

// ==== Switch task (handles delivery and ACKs) ====
void vSwitchTask(void *pvParams) {
    void *msg;
    while (1) {
        TickType_t now = xTaskGetTickCount();

        // Check retry buffer for timeouts
        for (int i = 0; i < RETRY_LIST_SIZE; i++) {
            if (retryBuffer[i].active && now >= retryBuffer[i].timeout_tick) {
                resendPacket(&retryBuffer[i]);
            }
        }

        // Process data packets
        if (xQueueReceive(xSwitchQueue, &msg, pdMS_TO_TICKS(10)) == pdPASS) {
            Packet *pkt = (Packet *)msg;

            // Simulate random drop
            if ((rand() % 100) < P_drop) {
                free(pkt);
                packetsDropped++;
                continue;
            }

            // Simulate delay
            TickType_t delay = pdMS_TO_TICKS(PROPAGATION_DELAY_MS) +
                               pdMS_TO_TICKS((pkt->header.length * 8 * 1000) / LINK_CAPACITY);
            vTaskDelay(delay);

            // Forward to the appropriate receiver
            QueueHandle_t destQ = (pkt->header.destination == 3) ? xReceiver3Queue : xReceiver4Queue;
            xQueueSend(destQ, &pkt, portMAX_DELAY);
            addToRetryList(pkt, pkt->header.destination);
        }

        // Process ACK packets
        AckPacket *ack;
        if (xQueueReceive(xAckQueue, &ack, 0) == pdPASS) {
            TickType_t ackDelay = pdMS_TO_TICKS(PROPAGATION_DELAY_MS) +
                                  pdMS_TO_TICKS((ACK_LENGTH * 8 * 1000) / LINK_CAPACITY);
            vTaskDelay(ackDelay);

            // Simulate ACK loss
            if ((rand() % 100) < 1) {
                free(ack);
                continue;
            }

            now = xTaskGetTickCount();
            for (int i = 0; i < RETRY_LIST_SIZE; i++) {
                if (retryBuffer[i].active &&
                    retryBuffer[i].data_packet->header.sender_id == ack->destination &&
                    retryBuffer[i].data_packet->header.destination == ack->source &&
                    retryBuffer[i].data_packet->header.sequence_num == ack->sequence_num) {

                    if (now <= retryBuffer[i].timeout_tick) {
                        packetsReceived[ack->source - 3]++;
                    }

                    // Slide Go-Back-N window forward
                    SenderWindow *window = (ack->destination == 1) ? &sender1Window : &sender2Window;
                    uint32_t *base = (ack->source == 3) ? &window->base_seq_3 : &window->base_seq_4;
                    *base = ack->sequence_num + 1;

                    free(retryBuffer[i].data_packet);
                    retryBuffer[i].active = pdFALSE;
                    break;
                }
            }
            free(ack);
        }
    }
}

// ==== Statistics output task ====
void vStatsTask(void *pvParameters) {
    const double avgPacketSize = (MIN_PACKET_SIZE + MAX_PACKET_SIZE) / 2.0;
    while (1) {
        // Suspended = generated but not yet resolved (not received, dropped, or failed)
        int suspended = packetsGenerated[0] + packetsGenerated[1] - packetsReceived[0] - packetsReceived[1] - packetsDropped - packetsFailedToDeliver;
        uint32_t totalPacketsReceived = packetsReceived[0] + packetsReceived[1];
        uint32_t totalBytesReceived = (uint32_t)(totalPacketsReceived * avgPacketSize);

        // Output statistics
        printf("\n======== Communication Stats ========\n");
        printf("Window Size (N): %d\n", WINDOW_SIZE);
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

// ==== FreeRTOS required hooks ====
void vApplicationMallocFailedHook(void) { while (1); }
void vApplicationTickHook(void) {}
void vApplicationIdleHook(void) {}
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) { (void)xTask; (void)pcTaskName; while (1); }

// ==== Main function ====
int main(void) {
    srand((unsigned int)time(NULL));

    // Create communication queues
    sender1Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    sender2Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    xSwitchQueue = xQueueCreate(QUEUE_LENGTH, sizeof(void *));
    xReceiver3Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    xReceiver4Queue = xQueueCreate(QUEUE_LENGTH, sizeof(Packet *));
    xAckQueue = xQueueCreate(QUEUE_LENGTH, sizeof(AckPacket *));

    // Create and start periodic packet generation timers
    sender1Timer = xTimerCreate("S1Gen", getRandomInterval(), pdTRUE, (void *)1, senderCallback);
    sender2Timer = xTimerCreate("S2Gen", getRandomInterval(), pdTRUE, (void *)2, senderCallback);
    xTimerStart(sender1Timer, 0);
    xTimerStart(sender2Timer, 0);

    // Launch RTOS tasks
    xTaskCreate(vSenderTask, "Sender1", 2000, sender1Queue, 3, NULL);
    xTaskCreate(vSenderTask, "Sender2", 2000, sender2Queue, 3, NULL);
    xTaskCreate(vSwitchTask, "Switch", 2000, NULL, 3, NULL);
    xTaskCreate(vReceiverTask, "Receiver3", 2000, xReceiver3Queue, 2, NULL);
    xTaskCreate(vReceiverTask, "Receiver4", 2000, xReceiver4Queue, 2, NULL);
    xTaskCreate(vStatsTask, "Stats", 2000, NULL, 1, NULL);

    // Start FreeRTOS scheduler
    vTaskStartScheduler();
    while (1);
    return 0;
}
