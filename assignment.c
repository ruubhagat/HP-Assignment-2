#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For usleep
#include <ctype.h>  // For isspace

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

// Cache line states MUST match this order for print formatting later
typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;
// Directory states MUST match this order for print formatting later
typedef enum { EM, S, U } directoryEntryState;

typedef enum {
    READ_REQUEST,
    WRITE_REQUEST,
    REPLY_RD,
    REPLY_WR,
    REPLY_ID,
    INV,
    UPGRADE,
    WRITEBACK_INV,
    WRITEBACK_INT,
    FLUSH,
    FLUSH_INVACK,
    EVICT_SHARED,
    EVICT_MODIFIED
} transactionType;

typedef struct instruction {
    byte type;
    byte address;
    byte value;
} instruction;

typedef struct cacheLine {
    byte address;
    byte value;
    cacheLineState state;
} cacheLine;

typedef struct directoryEntry {
    byte bitVector;
    directoryEntryState state;
} directoryEntry;

typedef struct message {
    transactionType type;
    int sender;
    byte address;
    byte value;
    byte bitVector;      // Also used in REPLY_RD to signal Exclusive (2) vs Shared (0)
    int secondReceiver;
    directoryEntryState dirState;
} message;

typedef struct messageBuffer {
    message queue[ MSG_BUFFER_SIZE ];
    int head;
    int tail;
    int count;
} messageBuffer;

typedef struct processorNode {
    cacheLine cache[ CACHE_SIZE ];
    byte memory[ MEM_SIZE ];
    directoryEntry directory[ MEM_SIZE ];
    instruction instructions[ MAX_INSTR_NUM ];
    int instructionCount;
    byte pendingWriteValue;
    byte waitingForReply;
    int outstandingMsgs; // Not strictly used currently, but available

} processorNode;

// Function Prototypes
void initializeProcessor( int threadId, processorNode *node, char *dirName );
void sendMessage( int receiver, message msg );
void handleCacheReplacement( int sender, cacheLine oldCacheLine );
void printProcessorState( int processorId, processorNode node );


// Global Shared Variables
messageBuffer messageBuffers[ NUM_PROCS ];
omp_lock_t msgBufferLocks[ NUM_PROCS ];

// Helper Functions
int isBitSet(byte bitVector, int procId) {
    return (bitVector >> procId) & 1;
}

int findOwner(byte bitVector) {
    for (int i = 0; i < NUM_PROCS; ++i) {
        if ((bitVector >> i) & 1) {
            return i;
        }
    }
    return -1; // Indicates no single bit set (error in EM state)
}

int countSharers(byte bitVector) {
    int count = 0;
    for (int i = 0; i < NUM_PROCS; ++i) {
        if ((bitVector >> i) & 1) {
            count++;
        }
    }
    return count;
}


// Main Function
int main( int argc, char * argv[] ) {
    if (argc < 2) {
        fprintf( stderr, "Usage: %s <test_directory>\n", argv[0] );
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];

    omp_set_num_threads( NUM_PROCS );

    // Initialize locks before parallel region
    for ( int i = 0; i < NUM_PROCS; i++ ) {
        messageBuffers[ i ].count = 0;
        messageBuffers[ i ].head = 0;
        messageBuffers[ i ].tail = 0;
        omp_init_lock(&msgBufferLocks[ i ]);
    }

    #pragma omp parallel // Start parallel execution
    {
        int threadId = omp_get_thread_num();
        processorNode node; // Node is private to each thread
        message msg;
        message msgReply;
        instruction instr;
        int instructionIdx = -1;
        int instructions_done = 0; // Flag for when instructions are finished
        node.waitingForReply = 0;  // Initialize private state
        node.outstandingMsgs = 0;

        // Initialize this processor node
        initializeProcessor( threadId, &node, dirName );

        #pragma omp barrier // Wait for all threads to initialize

        // Main Simulation Loop
        while ( 1 ) {
            int processed_message_this_cycle = 0;

            // --- Step 1: Process Incoming Messages ---
            omp_set_lock(&msgBufferLocks[threadId]); // Lock own buffer to read
            while ( messageBuffers[ threadId ].count > 0 ) {
                processed_message_this_cycle = 1;

                int head = messageBuffers[ threadId ].head;
                msg = messageBuffers[ threadId ].queue[ head ]; // Copy message
                messageBuffers[ threadId ].head = ( head + 1 ) % MSG_BUFFER_SIZE;
                messageBuffers[ threadId ].count--;

                omp_unset_lock(&msgBufferLocks[threadId]); // Release lock before processing

                #ifdef DEBUG_MSG
                printf( "Processor %d msg from: %d, type: %d, address: 0x%02X, val: %d, vec: 0x%02X, rcv2: %d\n",
                        threadId, msg.sender, msg.type, msg.address, msg.value, msg.bitVector, msg.secondReceiver);
                #endif

                // Common variables for message handling
                byte procNodeAddr = msg.address >> 4; // Home node ID
                byte memBlockAddr = msg.address & 0x0F; // Memory index at home
                byte cacheIndex = msg.address % CACHE_SIZE; // Cache index (local)
                cacheLine *line = &node.cache[ cacheIndex ];
                directoryEntry *dirEntry = NULL; // Directory entry pointer (null if not home node)
                if (threadId == procNodeAddr) { // Access directory only if this is the home node
                   dirEntry = &node.directory[ memBlockAddr ];
                }

                // Process message based on type
                switch ( msg.type ) {
                     case READ_REQUEST: // Received at Home Node
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);
                        msgReply.type = REPLY_RD;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.secondReceiver = -1;

                        switch(dirEntry->state) {
                            case U: // Unowned -> Exclusive (EM)
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);
                                msgReply.value = node.memory[memBlockAddr];
                                msgReply.bitVector = 2; // <<<--- Signal EXCLUSIVE with 2
                                sendMessage(msg.sender, msgReply);
                                break;
                            case S: // Shared -> Shared (add requestor)
                                dirEntry->bitVector |= (1 << msg.sender);
                                msgReply.value = node.memory[memBlockAddr];
                                msgReply.bitVector = 0; // <<<--- Signal SHARED with 0
                                sendMessage(msg.sender, msgReply);
                                break;
                            case EM: // Exclusive/Modified -> Shared (intervention needed)
                                {
                                    int ownerId = findOwner(dirEntry->bitVector);
                                    assert(ownerId != -1);

                                    if (ownerId == msg.sender) {
                                         // Read request from current owner - should be a hit locally for them.
                                         #ifdef DEBUG_MSG
                                         printf("Processor %d (Home) received READ_REQUEST for 0x%02X from current owner %d! Sending data, state remains EM.\n", threadId, msg.address, msg.sender);
                                         #endif
                                         msgReply.value = node.memory[memBlockAddr]; // Data might be stale if M
                                         msgReply.bitVector = 2; // Signal EXCLUSIVE
                                         sendMessage(msg.sender, msgReply);
                                    } else { // Normal intervention
                                        message forwardMsg;
                                        forwardMsg.type = WRITEBACK_INT;
                                        forwardMsg.sender = threadId;
                                        forwardMsg.address = msg.address;
                                        forwardMsg.secondReceiver = msg.sender; // Original requestor
                                        sendMessage(ownerId, forwardMsg); // Send to owner

                                        dirEntry->state = S; // State becomes Shared
                                        dirEntry->bitVector |= (1 << msg.sender); // Add requestor to vector
                                    }
                                }
                                break;
                        }
                        break;

                    case REPLY_RD: // Received at Requesting Node
                        if (line->address != 0xFF && line->address != msg.address && line->state != INVALID) {
                           handleCacheReplacement(threadId, *line); // Evict old line if needed
                        }
                        line->address = msg.address;
                        line->value = msg.value;
                        // Set state based on signal from Home Node (2=Exclusive, 0=Shared)
                        line->state = (msg.bitVector == 2) ? EXCLUSIVE : SHARED; // <<<--- Modified logic
                        node.waitingForReply = 0; // Request satisfied
                        break;

                    case WRITEBACK_INT: // Received at Old Owner Node (Holding block in E or M)
                        // Relaxed check: Verify we have the block in M or E state.
                        if (line->address == msg.address && (line->state == MODIFIED || line->state == EXCLUSIVE)) {
                            msgReply.type = FLUSH;
                            msgReply.sender = threadId; // Owner sends flush
                            msgReply.address = msg.address;
                            msgReply.value = line->value; // Flush current value
                            msgReply.secondReceiver = msg.secondReceiver; // Original requestor ID

                            sendMessage(procNodeAddr, msgReply); // Send to home

                            if (msg.secondReceiver != procNodeAddr) { // Also send to requestor if different
                               sendMessage(msg.secondReceiver, msgReply);
                            }

                            line->state = SHARED; // Downgrade local state
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d received WRITEBACK_INT for 0x%02X but line state is %d (addr 0x%02X). Ignoring FLUSH.\n",
                                    threadId, msg.address, line->state, line->address);
                             #endif
                             // Cannot fulfill - potentially stale request due to race
                        }
                        break;

                    case FLUSH: // Received at Home Node OR at Requesting Node
                        if (threadId == procNodeAddr) { // At Home Node
                            assert(dirEntry != NULL);
                            node.memory[memBlockAddr] = msg.value; // Update memory
                             // Check state consistency (optional, might fail in races)
                             if (dirEntry->state != S) {
                                #ifdef DEBUG_MSG
                                printf("Processor %d (Home) received FLUSH for 0x%02X, expected state S but got %d. Vector=0x%02X. Updating mem only.\n",
                                       threadId, msg.address, dirEntry->state, dirEntry->bitVector);
                                #endif
                             }
                             // Ensure the participants are in the bitvector (might fail in races)
                             // assert(isBitSet(dirEntry->bitVector, msg.sender)); // sender = old owner
                             // assert(isBitSet(dirEntry->bitVector, msg.secondReceiver)); // rcv2 = requestor
                         }
                        if (threadId == msg.secondReceiver) { // At Requesting Node
                           if (line->address != 0xFF && line->address != msg.address && line->state != INVALID) {
                               handleCacheReplacement(threadId, *line);
                           }
                           line->address = msg.address;
                           line->value = msg.value;
                           line->state = SHARED; // Become shared
                           node.waitingForReply = 0; // Got data for read miss
                        }
                        break;

                    case UPGRADE: // Received at Home Node (Requesting write hit on S)
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);

                        // Relaxed check for state S
                        if (dirEntry->state == S) {
                            msgReply.type = REPLY_ID;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.bitVector = dirEntry->bitVector & ~(1 << msg.sender); // Other sharers
                            msgReply.secondReceiver = -1;
                            sendMessage(msg.sender, msgReply); // Send list to requestor

                            dirEntry->state = EM; // Become exclusive owner
                            dirEntry->bitVector = (1 << msg.sender);
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d (Home) received UPGRADE for 0x%02X from %d, but state is %d (expected S). Vector: 0x%02X\n",
                                    threadId, msg.address, msg.sender, dirEntry->state, dirEntry->bitVector);
                             #endif
                            // Handle unexpected states (e.g., if already EM or U due to race)
                            if (dirEntry->state == EM || dirEntry->state == U) {
                                // Promote/confirm sender as EM owner
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);
                                // Send empty REPLY_ID as ack
                                 msgReply.type = REPLY_ID;
                                 msgReply.sender = threadId;
                                 msgReply.address = msg.address;
                                 msgReply.bitVector = 0;
                                 msgReply.secondReceiver = -1;
                                 sendMessage(msg.sender, msgReply);
                            }
                        }
                        break;

                    case REPLY_ID: // Received at Requesting Node (who sent UPGRADE or WRITE_REQUEST(S))
                        // If this was a write miss where home was S, update cache line now
                        if (line->address == msg.address && line->state != MODIFIED) {
                             line->address = msg.address;
                             line->value = node.pendingWriteValue; // Use stored value
                             line->state = MODIFIED;
                        } else {
                            // If it was UPGRADE (hit on S), state should already be M
                             assert(line->address == msg.address && line->state == MODIFIED);
                        }

                        // Invalidate other sharers from the received list
                        for (int i = 0; i < NUM_PROCS; ++i) {
                            if (i != threadId && isBitSet(msg.bitVector, i)) {
                                message invMsg;
                                invMsg.type = INV;
                                invMsg.sender = threadId; // New owner sends invalidate
                                invMsg.address = msg.address;
                                invMsg.secondReceiver = -1;
                                sendMessage(i, invMsg);
                                #ifdef DEBUG_MSG
                                printf("Processor %d sent INV for 0x%02X to %d\n", threadId, msg.address, i);
                                #endif
                            }
                        }
                        node.waitingForReply = 0; // Request is complete
                        break;

                    case INV: // Received at a Sharer Node
                        // Invalidate if currently Shared or Exclusive
                        if (line->address == msg.address && (line->state == SHARED || line->state == EXCLUSIVE)) {
                            line->state = INVALID;
                             #ifdef DEBUG_MSG
                             printf("Processor %d invalidated 0x%02X due to INV from %d\n", threadId, msg.address, msg.sender);
                             #endif
                        }
                        break;

                    case WRITE_REQUEST: // Received at Home Node
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);

                        node.memory[memBlockAddr] = msg.value; // Update memory immediately

                        switch(dirEntry->state) {
                            case U: // Unowned -> Modified (EM)
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);

                                msgReply.type = REPLY_WR; // Ack write
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.secondReceiver = -1;
                                sendMessage(msg.sender, msgReply);
                                break;

                            case S: // Shared -> Modified (EM)
                                msgReply.type = REPLY_ID; // Send sharer list
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.bitVector = dirEntry->bitVector & ~(1 << msg.sender); // Exclude requestor
                                msgReply.secondReceiver = -1;
                                sendMessage(msg.sender, msgReply);

                                dirEntry->state = EM; // Become exclusive owner
                                dirEntry->bitVector = (1 << msg.sender);
                                break;

                            case EM: // EM -> EM (Writeback-Invalidate needed)
                                {
                                    int ownerId = findOwner(dirEntry->bitVector);
                                     assert(ownerId != -1);

                                     if (ownerId == msg.sender) { // Write request from owner? Error state.
                                          #ifdef DEBUG_MSG
                                          printf("Processor %d (Home) received WRITE_REQUEST for 0x%02X from current owner %d! Logic error?\n", threadId, msg.address, msg.sender);
                                          #endif
                                         // Treat as U->EM? Just Ack.
                                         msgReply.type = REPLY_WR;
                                         msgReply.sender = threadId;
                                         msgReply.address = msg.address;
                                         msgReply.secondReceiver = -1;
                                         sendMessage(msg.sender, msgReply);
                                         // State remains EM, owner msg.sender
                                     } else { // Normal intervention
                                        message forwardMsg;
                                        forwardMsg.type = WRITEBACK_INV;
                                        forwardMsg.sender = threadId;
                                        forwardMsg.address = msg.address;
                                        forwardMsg.secondReceiver = msg.sender; // New owner ID
                                        sendMessage(ownerId, forwardMsg); // Send to old owner

                                        // Update directory owner tentatively (will be confirmed by FLUSH_INVACK)
                                        dirEntry->bitVector = (1 << msg.sender);
                                        // State remains EM
                                     }
                                }
                                break;
                        }
                        break;

                    case REPLY_WR: // Received at Requesting Node (from Write Miss U -> EM)
                         assert(line->address == msg.address || line->address == 0xFF);

                        line->address = msg.address;
                        line->value = node.pendingWriteValue; // Use stored value
                        line->state = MODIFIED; // Become owner
                        node.waitingForReply = 0;
                        break;

                    case WRITEBACK_INV: // Received at Old Owner Node (holding E or M)
                        // Relaxed check
                        if (line->address == msg.address && (line->state == MODIFIED || line->state == EXCLUSIVE)) {
                             msgReply.type = FLUSH_INVACK;
                             msgReply.sender = threadId; // Old owner sends
                             msgReply.address = msg.address;
                             msgReply.value = line->value; // Flush current value
                             msgReply.secondReceiver = msg.secondReceiver; // New owner ID

                             sendMessage(procNodeAddr, msgReply); // Send to home

                             if (msg.secondReceiver != procNodeAddr) { // Also send to new owner if different
                                sendMessage(msg.secondReceiver, msgReply);
                             }

                             line->state = INVALID; // Invalidate local line
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d received WRITEBACK_INV for 0x%02X but line state is %d (addr 0x%02X). Ignoring FLUSH.\n",
                                    threadId, msg.address, line->state, line->address);
                             #endif
                             // Cannot fulfill potentially stale request
                        }
                        break;

                    case FLUSH_INVACK: // Received at Home Node OR at New Owner Node
                        if (threadId == procNodeAddr) { // At Home Node
                             assert(dirEntry != NULL);
                             node.memory[memBlockAddr] = msg.value; // Update memory
                             dirEntry->state = EM; // Confirm EM state
                             dirEntry->bitVector = (1 << msg.secondReceiver); // Confirm new owner
                        }

                        if (threadId == msg.secondReceiver) { // At New Owner Node (original requestor)
                            assert(line->address == msg.address || line->address == 0xFF); // Should be target or empty

                            line->address = msg.address;
                            line->value = msg.value; // Use flushed value (most recent)
                            line->state = MODIFIED; // Become owner
                            node.waitingForReply = 0; // Write request completed
                        }
                        break;

                    case EVICT_SHARED: // Received at Home Node OR at last remaining sharer
                        if (threadId == procNodeAddr) { // Received at Home node
                            assert(dirEntry != NULL);
                            // Check if sender still holds the block (it might have been invalidated)
                            if (isBitSet(dirEntry->bitVector, msg.sender)) {
                                dirEntry->bitVector &= ~(1 << msg.sender); // Remove sender

                                int remainingSharers = countSharers(dirEntry->bitVector);
                                if (remainingSharers == 0) { // No sharers left
                                    dirEntry->state = U;
                                } else if (remainingSharers == 1 && dirEntry->state == S) { // Only one left, was Shared
                                    dirEntry->state = EM; // Promote to EM
                                    int newOwnerId = findOwner(dirEntry->bitVector);
                                    if (newOwnerId != -1) {
                                        // Notify the remaining sharer to upgrade S -> E
                                        message upgradeNotifyMsg;
                                        upgradeNotifyMsg.type = EVICT_SHARED; // Re-use type for notification
                                        upgradeNotifyMsg.sender = threadId; // Home sends
                                        upgradeNotifyMsg.address = msg.address;
                                        sendMessage(newOwnerId, upgradeNotifyMsg);
                                         #ifdef DEBUG_MSG
                                         printf("Processor %d (Home) telling %d to upgrade 0x%02X to E\n", threadId, newOwnerId, msg.address);
                                         #endif
                                    }
                                } // Else (>1 sharer or was EM), state remains S or EM
                            }
                        } else { // Received at the potential new owner (last remaining sharer)
                             #ifdef DEBUG_MSG
                             printf("Processor %d received upgrade request 0x%02X from Home %d\n", threadId, msg.address, msg.sender);
                             #endif
                             // Verify sender is home node
                              if (msg.sender == procNodeAddr) {
                                // If we still have the block as Shared, upgrade to Exclusive
                                if (line->address == msg.address && line->state == SHARED) {
                                    line->state = EXCLUSIVE;
                                    #ifdef DEBUG_MSG
                                    printf("Processor %d upgraded 0x%02X from S to E\n", threadId, msg.address);
                                    #endif
                                }
                              } else { // Ignore if not from home node
                                #ifdef DEBUG_MSG
                                printf("Processor %d received EVICT_SHARED upgrade request for 0x%02X from non-home node %d. Ignoring.\n", threadId, msg.address, msg.sender);
                                #endif
                              }
                        }
                        break;

                    case EVICT_MODIFIED: // Received at Home Node
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);
                        node.memory[memBlockAddr] = msg.value; // Update memory with flushed value

                         // Check state consistency and clear owner
                         if(dirEntry->state == EM && isBitSet(dirEntry->bitVector, msg.sender)){
                             dirEntry->bitVector = 0;
                             dirEntry->state = U;
                         } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d (Home) received EVICT_MODIFIED for 0x%02X from %d, but state was %d/vector 0x%02X - Race condition?\n",
                                    threadId, msg.address, msg.sender, dirEntry->state, dirEntry->bitVector);
                             // Attempt recovery if possible: If sender is the only one set, clear anyway.
                             if (countSharers(dirEntry->bitVector) == 1 && isBitSet(dirEntry->bitVector, msg.sender)) {
                                dirEntry->bitVector = 0;
                                dirEntry->state = U;
                                 #ifdef DEBUG_MSG
                                 printf("Processor %d (Home) recovered EVICT_MODIFIED state to U.\n", threadId);
                                 #endif
                             }
                             #endif
                         }
                        break;

                    default:
                        fprintf(stderr, "Processor %d: Unknown message type %d\n", threadId, msg.type);
                        break;
                } // End switch(msg.type)

                 omp_set_lock(&msgBufferLocks[threadId]); // Re-acquire lock to check buffer count
            } // End while messages in buffer
            omp_unset_lock(&msgBufferLocks[threadId]);

            // Optional: Print state after processing messages if instructions are done
            if (instructions_done && processed_message_this_cycle) {
                 // printProcessorState(threadId, node); // Uncomment if needed per requirements update
            }

            // --- Step 2: Check if Waiting for Reply ---
            if (node.waitingForReply > 0) {
                // usleep(10); // Optional yield to reduce busy-waiting
                continue; // Skip instruction processing, go back to check messages
            }

            // --- Step 3: Check if Instructions are Done ---
            if (instructions_done) {
                 // Instructions finished, just process messages or idle
                 if (!processed_message_this_cycle) usleep(100); // Yield if nothing happened
                continue; // Loop back to check messages
            }

            // --- Step 4: Process Next Instruction ---
            if (instructionIdx < node.instructionCount - 1) {
                instructionIdx++;
                instr = node.instructions[ instructionIdx ];

                 #ifdef DEBUG_INSTR
                 printf("Processor %d: Executing instr %d: %c 0x%02X",
                        threadId, instructionIdx, instr.type, instr.address);
                 if (instr.type == 'W') {
                     printf(" value %d", instr.value);
                 }
                 printf("\n");
                 #endif

                byte procNodeAddr = instr.address >> 4;
                //byte memBlockAddr = instr.address & 0x0F; // Not needed for local instruction logic
                byte cacheIndex = instr.address % CACHE_SIZE;
                cacheLine *line = &node.cache[ cacheIndex ];

                // --- Process Read Instruction ---
                if ( instr.type == 'R' ) {
                    if (line->address == instr.address && line->state != INVALID) { // Cache Hit (M, E, or S)
                        #ifdef DEBUG_INSTR
                        printf("Processor %d: Read Hit for 0x%02X, State: %d, Value: %d\n", threadId, instr.address, line->state, line->value);
                        #endif
                        // No state change on read hit, value is available
                    } else { // Cache Miss (or Invalid line hit)
                        #ifdef DEBUG_INSTR
                        printf("Processor %d: Read Miss for 0x%02X\n", threadId, instr.address);
                        #endif
                        // 1. Handle replacement if the current line is valid but for a different address
                        if (line->address != 0xFF && line->state != INVALID) {
                             handleCacheReplacement(threadId, *line);
                        }
                        // 2. Send READ_REQUEST to home node
                        message requestMsg;
                        requestMsg.type = READ_REQUEST;
                        requestMsg.sender = threadId;
                        requestMsg.address = instr.address;
                        requestMsg.secondReceiver = -1;
                        sendMessage(procNodeAddr, requestMsg);
                        // 3. Set waiting flag
                        node.waitingForReply = 1;
                        // 4. Mark cache line as pending/invalid
                         line->state = INVALID;
                         line->address = instr.address; // Keep track of target address
                         line->value = 0; // Clear old value
                    }
                }
                // --- Process Write Instruction ---
                else { // Write Instruction ('W')
                    node.pendingWriteValue = instr.value; // Store value needed if miss/upgrade

                    if (line->address == instr.address && line->state != INVALID) { // Write Hit
                         #ifdef DEBUG_INSTR
                         printf("Processor %d: Write Hit for 0x%02X, State: %d\n", threadId, instr.address, line->state);
                         #endif
                         switch(line->state) {
                            case MODIFIED: // Hit on M
                            case EXCLUSIVE: // Hit on E
                                line->value = instr.value; // Write locally
                                line->state = MODIFIED;    // State becomes/remains M
                                // No network traffic, no waiting
                                break;

                            case SHARED:    // Hit on S -> Upgrade
                                #ifdef DEBUG_INSTR
                                printf("Processor %d: Write Hit on SHARED 0x%02X -> Sending UPGRADE\n", threadId, instr.address);
                                #endif
                                message upgradeMsg;
                                upgradeMsg.type = UPGRADE;
                                upgradeMsg.sender = threadId;
                                upgradeMsg.address = instr.address;
                                upgradeMsg.secondReceiver = -1;
                                sendMessage(procNodeAddr, upgradeMsg); // Send UPGRADE to Home
                                line->value = instr.value;             // Write locally
                                line->state = MODIFIED;                // State becomes M
                                node.waitingForReply = 1;              // Wait for REPLY_ID (invalidation list)
                                break;

                            default: // Should not happen (INVALID handled by outer 'else')
                                #ifdef DEBUG_INSTR
                                fprintf(stderr, "Processor %d: Write Hit in unexpected state %d for 0x%02X\n", threadId, line->state, line->address);
                                #endif
                                break;
                         }

                    } else { // Write Miss (or Invalid line hit)
                        #ifdef DEBUG_INSTR
                        printf("Processor %d: Write Miss for 0x%02X\n", threadId, instr.address);
                        #endif
                        // 1. Handle replacement if the current line is valid but for a different address
                        if (line->address != 0xFF && line->state != INVALID) {
                             handleCacheReplacement(threadId, *line);
                        }
                        // 2. Send WRITE_REQUEST to home node
                        message requestMsg;
                        requestMsg.type = WRITE_REQUEST;
                        requestMsg.sender = threadId;
                        requestMsg.address = instr.address;
                        requestMsg.value = instr.value; // Include value for home node
                        requestMsg.secondReceiver = -1;
                        sendMessage(procNodeAddr, requestMsg);
                        // 3. Set waiting flag
                        node.waitingForReply = 1;
                        // 4. Mark cache line as pending/invalid
                         line->state = INVALID;
                         line->address = instr.address; // Keep track of target address
                         line->value = 0; // Clear old value
                    } // End Write Miss Block
                } // End Write Instruction Block


            } else { // All instructions for this thread have been issued
                 if (!instructions_done) {
                     #ifdef DEBUG_INSTR
                     printf("Processor %d finished issuing instructions.\n", threadId);
                     #endif
                     instructions_done = 1;
                     // Final mandatory print state call as per original requirements
                     printProcessorState(threadId, node);
                 }
            } // End instruction processing block

        } // End while(1) loop
    } // End parallel region

    // Clean up locks after parallel region
    for ( int i = 0; i < NUM_PROCS; i++ ) {
        omp_destroy_lock(&msgBufferLocks[i]);
    }

    return EXIT_SUCCESS;
}

// Function to send a message safely
void sendMessage( int receiver, message msg ) {
    omp_set_lock(&msgBufferLocks[receiver]);

    // Check if the buffer is full (basic retry mechanism)
    if ( messageBuffers[receiver].count >= MSG_BUFFER_SIZE ) {
         fprintf(stderr, "WARNING: Processor %d: Message buffer for %d is full! Sender %d waiting.\n", omp_get_thread_num(), receiver, msg.sender);
         omp_unset_lock(&msgBufferLocks[receiver]); // Release lock while waiting
         while(1) {
             usleep(1000); // Wait 1ms
             omp_set_lock(&msgBufferLocks[receiver]);
             if (messageBuffers[receiver].count < MSG_BUFFER_SIZE) break; // Check again
             omp_unset_lock(&msgBufferLocks[receiver]);
         }
         // Re-acquired lock here
    }

    // Add message to the queue
    int tail = messageBuffers[receiver].tail;
    messageBuffers[receiver].queue[tail] = msg;
    messageBuffers[receiver].tail = (tail + 1) % MSG_BUFFER_SIZE;
    messageBuffers[receiver].count++;

    omp_unset_lock(&msgBufferLocks[receiver]);

    #ifdef DEBUG_MSG
     printf("Processor %d sent msg to: %d, type: %d, address: 0x%02X, val: %d, vec: 0x%02X, rcv2: %d\n",
             msg.sender, receiver, msg.type, msg.address, msg.value, msg.bitVector, msg.secondReceiver);
    #endif
}

// Function to handle cache line replacement notification
void handleCacheReplacement( int sender, cacheLine oldCacheLine ) {
    // Only handle valid lines being replaced
    if (oldCacheLine.state == INVALID || oldCacheLine.address == 0xFF) {
        return;
    }

    byte procNodeAddr = oldCacheLine.address >> 4; // Home node ID

    #ifdef DEBUG_INSTR
    printf("Processor %d: Replacing cache line for address 0x%02X (State: %d)\n", sender, oldCacheLine.address, oldCacheLine.state);
    #endif

    message evictMsg;
    evictMsg.sender = sender; // The node performing the eviction
    evictMsg.address = oldCacheLine.address;
    evictMsg.secondReceiver = -1; // Not used

    switch ( oldCacheLine.state ) {
        case EXCLUSIVE: // Treat Exclusive like Shared (clean eviction)
        case SHARED:
            evictMsg.type = EVICT_SHARED;
            // No value needed
            sendMessage(procNodeAddr, evictMsg);
            break;
        case MODIFIED:
            evictMsg.type = EVICT_MODIFIED;
            evictMsg.value = oldCacheLine.value; // Send dirty value
            sendMessage(procNodeAddr, evictMsg);
            break;
        case INVALID: // Should not happen due to initial check
            break;
    }
}

// Function to initialize a processor node
void initializeProcessor( int threadId, processorNode *node, char *dirName ) {
    // IMPORTANT: DO NOT MODIFY
    for ( int i = 0; i < MEM_SIZE; i++ ) {
        node->memory[ i ] = 20 * threadId + i;  // some initial value to mem block
        node->directory[ i ].bitVector = 0;     // no cache has this block at start
        node->directory[ i ].state = U;         // this block is in Unowned state
    }

    for ( int i = 0; i < CACHE_SIZE; i++ ) {
        node->cache[ i ].address = 0xFF;        // this address is invalid as we can
                                                // have a maximum of 8 nodes in the 
                                                // current implementation
        node->cache[ i ].value = 0;
        node->cache[ i ].state = INVALID;       // all cache lines are invalid
    }

    // Read instructions from file
    char filename[ 128 ];
    snprintf(filename, sizeof(filename), "tests/%s/core_%d.txt", dirName, threadId);
    FILE *file = fopen( filename, "r" );
    if ( !file ) {
        fprintf( stderr, "Error: could not open file %s\n", filename );
         perror("fopen"); // Print system error message
         exit(EXIT_FAILURE); // Terminate simulation if file can't be opened
    }

    char line[ 20 ];
    node->instructionCount = 0;
    while ( fgets( line, sizeof( line ), file ) &&
            node->instructionCount < MAX_INSTR_NUM ) {
        if ( line[ 0 ] == 'R' && line[ 1 ] == 'D' ) {
            sscanf( line, "RD %hhx",
                    &node->instructions[ node->instructionCount ].address );
            node->instructions[ node->instructionCount ].type = 'R';
            node->instructions[ node->instructionCount ].value = 0;
        } else if ( line[ 0 ] == 'W' && line[ 1 ] == 'R' ) {
            sscanf( line, "WR %hhx %hhu",
                    &node->instructions[ node->instructionCount ].address,
                    &node->instructions[ node->instructionCount ].value );
            node->instructions[ node->instructionCount ].type = 'W';
        }
        node->instructionCount++;
    }

    fclose( file );
    printf( "Processor %d initialized\n", threadId );
}

void printProcessorState(int processorId, processorNode node) {
    // IMPORTANT: DO NOT MODIFY
    static const char *cacheStateStr[] = { "MODIFIED", "EXCLUSIVE", "SHARED",
                                           "INVALID" };
    static const char *dirStateStr[] = { "EM", "S", "U" };

    char filename[32];
    snprintf(filename, sizeof(filename), "core_%d_output.txt", processorId);

    FILE *file = fopen(filename, "w");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        return;
    }

    fprintf(file, "=======================================\n");
    fprintf(file, " Processor Node: %d\n", processorId);
    fprintf(file, "=======================================\n\n");

    // Print memory state
    fprintf(file, "-------- Memory State --------\n");
    fprintf(file, "| Index | Address |   Value  |\n");
    fprintf(file, "|----------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i,
                node.memory[i]);
    }
    fprintf(file, "------------------------------\n\n");

    // Print directory state
    fprintf(file, "------------ Directory State ---------------\n");
    fprintf(file, "| Index | Address | State |    BitVector   |\n");
    fprintf(file, "|------------------------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x%08B   |\n",
                i, (processorId << 4) + i, dirStateStr[node.directory[i].state],
                node.directory[i].bitVector);
    }
    fprintf(file, "--------------------------------------------\n\n");
    
    // Print cache state
    fprintf(file, "------------ Cache State ----------------\n");
    fprintf(file, "| Index | Address | Value |    State    |\n");
    fprintf(file, "|---------------------------------------|\n");
    for (int i = 0; i < CACHE_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %3d  |  %8s \t|\n",
               i, node.cache[i].address, node.cache[i].value,
               cacheStateStr[node.cache[i].state]);
    }
    fprintf(file, "----------------------------------------\n\n");

    fclose(file);
}