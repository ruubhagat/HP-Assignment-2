#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;
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
    byte bitVector;
    int secondReceiver;

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
    int outstandingMsgs;

} processorNode;


void initializeProcessor( int threadId, processorNode *node, char *dirName );
void sendMessage( int receiver, message msg );
void handleCacheReplacement( int sender, cacheLine oldCacheLine );
void printProcessorState( int processorId, processorNode node );


messageBuffer messageBuffers[ NUM_PROCS ];
omp_lock_t msgBufferLocks[ NUM_PROCS ];


int isBitSet(byte bitVector, int procId) {
    return (bitVector >> procId) & 1;
}

int findOwner(byte bitVector) {
    for (int i = 0; i < NUM_PROCS; ++i) {
        if ((bitVector >> i) & 1) {
            return i;
        }
    }
    return -1;
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


int main( int argc, char * argv[] ) {
    if (argc < 2) {
        fprintf( stderr, "Usage: %s <test_directory>\n", argv[0] );
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];

    omp_set_num_threads( NUM_PROCS );


    for ( int i = 0; i < NUM_PROCS; i++ ) {
        messageBuffers[ i ].count = 0;
        messageBuffers[ i ].head = 0;
        messageBuffers[ i ].tail = 0;
        omp_init_lock(&msgBufferLocks[ i ]);
    }

    #pragma omp parallel
    {
        int threadId = omp_get_thread_num();
        processorNode node;
        message msg;
        message msgReply;
        instruction instr;
        int instructionIdx = -1;
        int instructions_done = 0;
        node.waitingForReply = 0;
        node.pendingWriteValue = 0;
        node.outstandingMsgs = 0;


        initializeProcessor( threadId, &node, dirName );

        #pragma omp barrier

        while ( 1 ) {
            int processed_message_this_cycle = 0;


            omp_set_lock(&msgBufferLocks[threadId]);
            while ( messageBuffers[ threadId ].count > 0 ) {
                processed_message_this_cycle = 1;

                int head = messageBuffers[ threadId ].head;
                msg = messageBuffers[ threadId ].queue[ head ];

                messageBuffers[ threadId ].count--;
                messageBuffers[ threadId ].head = ( head + 1 ) % MSG_BUFFER_SIZE;


                omp_unset_lock(&msgBufferLocks[threadId]);

                #ifdef DEBUG_MSG

                printf( "Processor %d msg from: %d, type: %d, address: 0x%02X\n",
                        threadId, msg.sender, msg.type, msg.address);
                #endif


                byte procNodeAddr = msg.address >> 4;
                byte memBlockAddr = msg.address & 0x0F;
                byte cacheIndex = msg.address % CACHE_SIZE;
                cacheLine *line = &node.cache[ cacheIndex ];
                directoryEntry *dirEntry = NULL;
                if (threadId == procNodeAddr) {
                   dirEntry = &node.directory[ memBlockAddr ];
                }


                switch ( msg.type ) {
                     case READ_REQUEST:
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);
                        msgReply.type = REPLY_RD;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.secondReceiver = -1;

                        switch(dirEntry->state) {
                            case U:
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);
                                msgReply.value = node.memory[memBlockAddr];
                                msgReply.bitVector = 2;
                                sendMessage(msg.sender, msgReply);
                                break;
                            case S:
                                dirEntry->bitVector |= (1 << msg.sender);
                                msgReply.value = node.memory[memBlockAddr];
                                msgReply.bitVector = 0;
                                sendMessage(msg.sender, msgReply);
                                break;
                            case EM:
                                {
                                    int ownerId = findOwner(dirEntry->bitVector);
                                    assert(ownerId != -1);

                                    if (ownerId == msg.sender) {
                                         #ifdef DEBUG_MSG
                                         printf("Processor %d (Home) received READ_REQUEST for 0x%02X from current owner %d! Sending data, state remains EM.\n", threadId, msg.address, msg.sender);
                                         #endif
                                         msgReply.value = node.memory[memBlockAddr];
                                         msgReply.bitVector = 2;
                                         sendMessage(msg.sender, msgReply);
                                    } else {
                                        message forwardMsg;
                                        forwardMsg.type = WRITEBACK_INT;
                                        forwardMsg.sender = threadId;
                                        forwardMsg.address = msg.address;
                                        forwardMsg.secondReceiver = msg.sender;
                                        sendMessage(ownerId, forwardMsg);

                                        dirEntry->state = S;
                                        dirEntry->bitVector |= (1 << msg.sender);
                                    }
                                }
                                break;
                        }
                        break;

                    case REPLY_RD:

                        if (line->address != 0xFF && line->address != msg.address && line->state != INVALID) {
                           handleCacheReplacement(threadId, *line);
                        }
                        line->address = msg.address;
                        line->value = msg.value;
                        line->state = (msg.bitVector == 2) ? EXCLUSIVE : SHARED;
                        node.waitingForReply = 0;
                        break;

                    case WRITEBACK_INT:

                        if (line->address == msg.address && (line->state == MODIFIED || line->state == EXCLUSIVE)) {
                            msgReply.type = FLUSH;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.value = line->value;
                            msgReply.secondReceiver = msg.secondReceiver;

                            sendMessage(procNodeAddr, msgReply);

                            if (msg.secondReceiver != procNodeAddr) {
                               sendMessage(msg.secondReceiver, msgReply);
                            }

                            line->state = SHARED;
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d received WRITEBACK_INT for 0x%02X but line state is %d (addr 0x%02X). Ignoring FLUSH.\n",
                                    threadId, msg.address, line->state, line->address);
                             #endif
                        }
                        break;

                    case FLUSH:
                        if (threadId == procNodeAddr) {
                            assert(dirEntry != NULL);
                            node.memory[memBlockAddr] = msg.value;
                             if (dirEntry->state != S) {
                                #ifdef DEBUG_MSG
                                printf("Processor %d (Home) received FLUSH for 0x%02X, expected state S but got %d. Vector=0x%02X. Updating mem only.\n",
                                       threadId, msg.address, dirEntry->state, dirEntry->bitVector);
                                #endif
                             }


                         }
                        if (threadId == msg.secondReceiver) {

                           if (line->address != 0xFF && line->address != msg.address && line->state != INVALID) {
                               handleCacheReplacement(threadId, *line);
                           }
                           line->address = msg.address;
                           line->value = msg.value;
                           line->state = SHARED;
                           node.waitingForReply = 0;
                        }
                        break;

                    case UPGRADE:
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);

                        if (dirEntry->state == S) {
                            msgReply.type = REPLY_ID;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.bitVector = dirEntry->bitVector & ~(1 << msg.sender);
                            msgReply.secondReceiver = -1;
                            sendMessage(msg.sender, msgReply);

                            dirEntry->state = EM;
                            dirEntry->bitVector = (1 << msg.sender);
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d (Home) received UPGRADE for 0x%02X from %d, but state is %d (expected S). Vector: 0x%02X\n",
                                    threadId, msg.address, msg.sender, dirEntry->state, dirEntry->bitVector);
                             #endif
                            if (dirEntry->state == EM || dirEntry->state == U) {
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);
                                 msgReply.type = REPLY_ID;
                                 msgReply.sender = threadId;
                                 msgReply.address = msg.address;
                                 msgReply.bitVector = 0;
                                 msgReply.secondReceiver = -1;
                                 sendMessage(msg.sender, msgReply);
                            }
                        }
                        break;

                    case REPLY_ID:

                        if (line->address == msg.address && line->state != MODIFIED) {

                             line->address = msg.address;
                             line->value = node.pendingWriteValue;
                             line->state = MODIFIED;
                        } else if (line->address == msg.address && line->state == MODIFIED) {

                        } else {

                            #ifdef DEBUG_MSG
                            printf("Processor %d received REPLY_ID for 0x%02X but cache state is %d (addr 0x%02X). Ignoring INV.\n",
                                   threadId, msg.address, line->state, line->address);
                            #endif
                            node.waitingForReply = 0;
                            break;
                        }


                        for (int i = 0; i < NUM_PROCS; ++i) {
                            if (i != threadId && isBitSet(msg.bitVector, i)) {
                                message invMsg;
                                invMsg.type = INV;
                                invMsg.sender = threadId;
                                invMsg.address = msg.address;
                                invMsg.secondReceiver = -1;
                                sendMessage(i, invMsg);
                                #ifdef DEBUG_MSG
                                printf("Processor %d sent INV for 0x%02X to %d\n", threadId, msg.address, i);
                                #endif
                            }
                        }
                        node.waitingForReply = 0;
                        break;

                    case INV:
                        if (line->address == msg.address && (line->state == SHARED || line->state == EXCLUSIVE)) {
                            line->state = INVALID;
                             #ifdef DEBUG_MSG
                             printf("Processor %d invalidated 0x%02X due to INV from %d\n", threadId, msg.address, msg.sender);
                             #endif
                        }
                        break;

                    case WRITE_REQUEST:
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);

                        node.memory[memBlockAddr] = msg.value;

                        switch(dirEntry->state) {
                            case U:
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);

                                msgReply.type = REPLY_WR;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.secondReceiver = -1;
                                sendMessage(msg.sender, msgReply);
                                break;

                            case S:
                                msgReply.type = REPLY_ID;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.bitVector = dirEntry->bitVector & ~(1 << msg.sender);
                                msgReply.secondReceiver = -1;
                                sendMessage(msg.sender, msgReply);

                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);
                                break;

                            case EM:
                                {
                                    int ownerId = findOwner(dirEntry->bitVector);
                                     assert(ownerId != -1);

                                     if (ownerId == msg.sender) {
                                          #ifdef DEBUG_MSG
                                          printf("Processor %d (Home) received WRITE_REQUEST for 0x%02X from current owner %d! Logic error?\n", threadId, msg.address, msg.sender);
                                          #endif
                                         msgReply.type = REPLY_WR;
                                         msgReply.sender = threadId;
                                         msgReply.address = msg.address;
                                         msgReply.secondReceiver = -1;
                                         sendMessage(msg.sender, msgReply);

                                     } else {
                                        message forwardMsg;
                                        forwardMsg.type = WRITEBACK_INV;
                                        forwardMsg.sender = threadId;
                                        forwardMsg.address = msg.address;
                                        forwardMsg.secondReceiver = msg.sender;
                                        sendMessage(ownerId, forwardMsg);


                                        dirEntry->bitVector = (1 << msg.sender);

                                     }
                                }
                                break;
                        }
                        break;

                    case REPLY_WR:

                         if (line->address != 0xFF && line->address != msg.address && line->state != INVALID) {


                         }
                         assert(line->address == msg.address || line->address == 0xFF || line->state == INVALID);

                        line->address = msg.address;
                        line->value = node.pendingWriteValue;
                        line->state = MODIFIED;
                        node.waitingForReply = 0;
                        break;

                    case WRITEBACK_INV:

                        if (line->address == msg.address && (line->state == MODIFIED || line->state == EXCLUSIVE)) {
                             msgReply.type = FLUSH_INVACK;
                             msgReply.sender = threadId;
                             msgReply.address = msg.address;
                             msgReply.value = line->value;
                             msgReply.secondReceiver = msg.secondReceiver;

                             sendMessage(procNodeAddr, msgReply);

                             if (msg.secondReceiver != procNodeAddr) {
                                sendMessage(msg.secondReceiver, msgReply);
                             }

                             line->state = INVALID;
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d received WRITEBACK_INV for 0x%02X but line state is %d (addr 0x%02X). Ignoring FLUSH.\n",
                                    threadId, msg.address, line->state, line->address);
                             #endif
                        }
                        break;

                    case FLUSH_INVACK:
                        if (threadId == procNodeAddr) {
                             assert(dirEntry != NULL);
                             node.memory[memBlockAddr] = msg.value;
                             dirEntry->state = EM;
                             dirEntry->bitVector = (1 << msg.secondReceiver);
                        }

                        if (threadId == msg.secondReceiver) {

                           if (line->address != 0xFF && line->address != msg.address && line->state != INVALID) {


                           }
                            assert(line->address == msg.address || line->address == 0xFF || line->state == INVALID);

                            line->address = msg.address;
                            line->value = msg.value;
                            line->state = MODIFIED;
                            node.waitingForReply = 0;
                        }
                        break;

                    case EVICT_SHARED:
                        if (threadId == procNodeAddr) {
                            assert(dirEntry != NULL);
                            if (isBitSet(dirEntry->bitVector, msg.sender)) {
                                dirEntry->bitVector &= ~(1 << msg.sender);

                                int remainingSharers = countSharers(dirEntry->bitVector);
                                if (remainingSharers == 0) {
                                    dirEntry->state = U;
                                } else if (remainingSharers == 1 && dirEntry->state == S) {
                                    dirEntry->state = EM;
                                    int newOwnerId = findOwner(dirEntry->bitVector);
                                    if (newOwnerId != -1) {
                                        message upgradeNotifyMsg;
                                        upgradeNotifyMsg.type = EVICT_SHARED;
                                        upgradeNotifyMsg.sender = threadId;
                                        upgradeNotifyMsg.address = msg.address;
                                        sendMessage(newOwnerId, upgradeNotifyMsg);
                                         #ifdef DEBUG_MSG
                                         printf("Processor %d (Home) telling %d to upgrade 0x%02X to E\n", threadId, newOwnerId, msg.address);
                                         #endif
                                    }
                                }
                            }
                        } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d received upgrade request 0x%02X from Home %d\n", threadId, msg.address, msg.sender);
                             #endif
                              if (msg.sender == procNodeAddr) {
                                if (line->address == msg.address && line->state == SHARED) {
                                    line->state = EXCLUSIVE;
                                    #ifdef DEBUG_MSG
                                    printf("Processor %d upgraded 0x%02X from S to E\n", threadId, msg.address);
                                    #endif
                                }
                              } else {
                                #ifdef DEBUG_MSG
                                printf("Processor %d received EVICT_SHARED upgrade request for 0x%02X from non-home node %d. Ignoring.\n", threadId, msg.address, msg.sender);
                                #endif
                              }
                        }
                        break;

                    case EVICT_MODIFIED:
                        assert(threadId == procNodeAddr);
                        assert(dirEntry != NULL);
                        node.memory[memBlockAddr] = msg.value;
                         if(dirEntry->state == EM && isBitSet(dirEntry->bitVector, msg.sender)){
                             dirEntry->bitVector = 0;
                             dirEntry->state = U;
                         } else {
                             #ifdef DEBUG_MSG
                             printf("Processor %d (Home) received EVICT_MODIFIED for 0x%02X from %d, but state was %d/vector 0x%02X - Race condition?\n",
                                    threadId, msg.address, msg.sender, dirEntry->state, dirEntry->bitVector);
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
                }

                 omp_set_lock(&msgBufferLocks[threadId]);
            }
            omp_unset_lock(&msgBufferLocks[threadId]);


            if (instructions_done && processed_message_this_cycle) {

            }


            if (node.waitingForReply > 0) {

                continue;
            }


            if (instructions_done) {
                 if (!processed_message_this_cycle) usleep(100);
                continue;
            }


            if (instructionIdx < node.instructionCount - 1) {
                instructionIdx++;
                instr = node.instructions[ instructionIdx ];


                 #ifdef DEBUG_INSTR
                 printf("Processor %d: instr type=%c, address=0x%02X, value=%d\n",
                        threadId, instr.type, instr.address, instr.value );
                 #endif



                byte procNodeAddr = instr.address >> 4;
                byte cacheIndex = instr.address % CACHE_SIZE;
                cacheLine *line = &node.cache[ cacheIndex ];


                if ( instr.type == 'R' ) {
                    if (line->address == instr.address && line->state != INVALID) {
                        #ifdef DEBUG_INSTR
                        printf("Processor %d: Read Hit for 0x%02X, State: %d, Value: %d\n", threadId, instr.address, line->state, line->value);
                        #endif
                    } else {
                        #ifdef DEBUG_INSTR
                        printf("Processor %d: Read Miss for 0x%02X\n", threadId, instr.address);
                        #endif
                        if (line->address != 0xFF && line->state != INVALID) {
                             handleCacheReplacement(threadId, *line);
                        }
                        message requestMsg;
                        requestMsg.type = READ_REQUEST;
                        requestMsg.sender = threadId;
                        requestMsg.address = instr.address;
                        requestMsg.secondReceiver = -1;
                        sendMessage(procNodeAddr, requestMsg);
                        node.waitingForReply = 1;
                         line->state = INVALID;
                         line->address = instr.address;
                         line->value = 0;
                    }
                }

                else {
                    node.pendingWriteValue = instr.value;

                    if (line->address == instr.address && line->state != INVALID) {
                         #ifdef DEBUG_INSTR
                         printf("Processor %d: Write Hit for 0x%02X, State: %d\n", threadId, instr.address, line->state);
                         #endif
                         switch(line->state) {
                            case MODIFIED:
                            case EXCLUSIVE:
                                line->value = instr.value;
                                line->state = MODIFIED;

                                break;
                            case SHARED:
                                #ifdef DEBUG_INSTR
                                printf("Processor %d: Write Hit on SHARED 0x%02X -> Sending UPGRADE\n", threadId, instr.address);
                                #endif
                                message upgradeMsg;
                                upgradeMsg.type = UPGRADE;
                                upgradeMsg.sender = threadId;
                                upgradeMsg.address = instr.address;
                                upgradeMsg.secondReceiver = -1;
                                sendMessage(procNodeAddr, upgradeMsg);
                                line->value = instr.value;
                                line->state = MODIFIED;
                                node.waitingForReply = 1;
                                break;
                            default:
                                #ifdef DEBUG_INSTR
                                fprintf(stderr, "Processor %d: Write Hit in unexpected state %d for 0x%02X\n", threadId, line->state, line->address);
                                #endif
                                break;
                         }
                    } else {
                        #ifdef DEBUG_INSTR
                        printf("Processor %d: Write Miss for 0x%02X\n", threadId, instr.address);
                        #endif
                        if (line->address != 0xFF && line->state != INVALID) {
                             handleCacheReplacement(threadId, *line);
                        }
                        message requestMsg;
                        requestMsg.type = WRITE_REQUEST;
                        requestMsg.sender = threadId;
                        requestMsg.address = instr.address;
                        requestMsg.value = instr.value;
                        requestMsg.secondReceiver = -1;
                        sendMessage(procNodeAddr, requestMsg);
                        node.waitingForReply = 1;
                         line->state = INVALID;
                         line->address = instr.address;
                         line->value = 0;
                    }
                }


            } else {
                 if (!instructions_done) {
                     #ifdef DEBUG_INSTR
                     printf("Processor %d finished issuing instructions.\n", threadId);
                     #endif
                     instructions_done = 1;

                     printProcessorState(threadId, node);
                 }
            }

        }
    }


    for ( int i = 0; i < NUM_PROCS; i++ ) {
        omp_destroy_lock(&msgBufferLocks[i]);
    }

    return EXIT_SUCCESS;
}


void sendMessage( int receiver, message msg ) {
    omp_set_lock(&msgBufferLocks[receiver]);


    if ( messageBuffers[receiver].count >= MSG_BUFFER_SIZE ) {
         fprintf(stderr, "WARNING: Processor %d: Message buffer for %d is full! Sender %d waiting.\n", omp_get_thread_num(), receiver, msg.sender);
         omp_unset_lock(&msgBufferLocks[receiver]);
         while(1) {
             usleep(1000);
             omp_set_lock(&msgBufferLocks[receiver]);
             if (messageBuffers[receiver].count < MSG_BUFFER_SIZE) break;
             omp_unset_lock(&msgBufferLocks[receiver]);
         }
    }


    int tail = messageBuffers[receiver].tail;
    messageBuffers[receiver].queue[tail] = msg;
    messageBuffers[receiver].tail = (tail + 1) % MSG_BUFFER_SIZE;
    messageBuffers[receiver].count++;

    omp_unset_lock(&msgBufferLocks[receiver]);

    #ifdef DEBUG_MSG

     printf("Processor %d sent msg to: %d, type: %d, address: 0x%02X\n",
             msg.sender, receiver, msg.type, msg.address);
    #endif
}


void handleCacheReplacement( int sender, cacheLine oldCacheLine ) {

    if (oldCacheLine.state == INVALID || oldCacheLine.address == 0xFF) {
        return;
    }

    byte procNodeAddr = oldCacheLine.address >> 4;

    #ifdef DEBUG_INSTR
    printf("Processor %d: Replacing cache line for address 0x%02X (State: %d)\n", sender, oldCacheLine.address, oldCacheLine.state);
    #endif

    message evictMsg;
    evictMsg.sender = sender;
    evictMsg.address = oldCacheLine.address;
    evictMsg.secondReceiver = -1;

    switch ( oldCacheLine.state ) {
        case EXCLUSIVE:
        case SHARED:
            evictMsg.type = EVICT_SHARED;
            sendMessage(procNodeAddr, evictMsg);
            break;
        case MODIFIED:
            evictMsg.type = EVICT_MODIFIED;
            evictMsg.value = oldCacheLine.value;
            sendMessage(procNodeAddr, evictMsg);
            break;
        case INVALID:
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
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x%08X   |\n",
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