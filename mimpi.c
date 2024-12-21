/**
 * This file is for implementation of MIMPI library.
 * */

#include "channel.h"
#include "mimpi.h"
#include "mimpi_common.h"
#include <pthread.h>
#include <sys/param.h>
#include <stdatomic.h>

#define MIMPI_CHANNEL_BUF 512
#define GROUP_BEGIN -2
#define GROUP_END -3
#define FINALIZE_BEGIN -1984
#define FINALIZE_END -4891
#define GROUP_FAIL -8

#define RECV_ASK -4
#define RECV_ANS -5
#define RECEIVED -6

struct MIMPI_Message{
    int source, tag, count;
    pthread_mutex_t is_buffered; // mutex to wait if the message is still being buffered
    void *buffer; // pointer to where the received data is stored
};
typedef struct MIMPI_Message MIMPI_Message;

inline static bool match(MIMPI_Message *a, MIMPI_Message *b) {
    return (((a->tag == 0 && b->tag > 0) || a->tag == b->tag) 
         && a->source == b->source 
         && a->count == b->count);
}

typedef struct MIMPI_Node MIMPI_Node;
struct MIMPI_Node {
    MIMPI_Message *msg;
    MIMPI_Node *prev, *next;
};

inline static MIMPI_Node* new_MIMPI_Node() {
    MIMPI_Node *node = malloc(sizeof(MIMPI_Node));
    ASSERT_NOT_NULL(node);
    ASSERT_NOT_NULL(node->msg = malloc(sizeof(MIMPI_Message)));

    pthread_mutexattr_t attr;
    ASSERT_ZERO(pthread_mutexattr_init(&attr));
    ASSERT_ZERO(pthread_mutex_init(&node->msg->is_buffered, &attr));
    ASSERT_ZERO(pthread_mutexattr_destroy(&attr));

    return node;
}

inline static void free_MIMPI_Node(MIMPI_Node *node) {
    if (node != NULL) {
        if (node->msg != NULL) {
            ASSERT_ZERO(pthread_mutex_destroy(&node->msg->is_buffered));
            free(node->msg->buffer);
        }
        free(node->msg);
    }
    free(node);
}

typedef struct MIMPI_Queue MIMPI_Queue;
struct MIMPI_Queue {
    pthread_mutex_t mutex;
    MIMPI_Node *begin, *end;
};


#define ASSERT_MIMPI_RECV_OK(expr)              \
    if (expr == MIMPI_ERROR_REMOTE_FINISHED)    \
        return MIMPI_ERROR_REMOTE_FINISHED;     


static MIMPI_Queue queue;
static MIMPI_Message *msg_pattern = NULL; // message we are waiting for
static atomic_bool found_matching_msg = true;
static pthread_cond_t matched_msg;

static int world_size, my_rank;
static int *write_fd, *read_fd;
static pthread_t threads[16];
static bool left_MIMPI_block[16];
static bool group_failed = false; // doesnt need to be atomic


// msg metadata - tag 4B, count 4B
static void* MIMPI_Receiver(void* receiving_from) {

    int *result = malloc(sizeof(int));
    ASSERT_NOT_NULL(result);
    *result = 0;

    int proc = *(int*)(receiving_from);
    free(receiving_from);

    const int meta_size = sizeof(int)*2;
    int meta_buf[2];

    while (1) {
        // read metadata before reading data - tag and count
        ssize_t read_len = chrecv(read_fd[proc], meta_buf, meta_size);
        if (read_len == 0) {
            return result;    
        }

        int tag = meta_buf[0];
        if (tag < 0) {
            if (tag == -1) {
                return result;
            }
            else if (tag == -7) {
                left_MIMPI_block[proc] = 1;
                ASSERT_ZERO(pthread_cond_signal(&matched_msg));
                continue;
            }
            else if (tag == GROUP_FAIL) {
                if (!group_failed) {
                    group_failed = true;
                    ASSERT_ZERO(pthread_cond_signal(&matched_msg));
                    const int l_child = (my_rank*2)-1;
                    const int r_child = l_child+1;
                    if (l_child < world_size) {
                        MIMPI_Send(NULL, 0, l_child, GROUP_FAIL);
                        if (r_child < world_size) {
                            MIMPI_Send(NULL, 0, r_child, GROUP_FAIL);
                        }
                    }
                }
                continue;
            }
        }
        
        MIMPI_Node *new_node = new_MIMPI_Node();
        MIMPI_Message *new_msg = new_node->msg;

        // lock it because the message isn't buffered
        ASSERT_ZERO(pthread_mutex_lock(&new_msg->is_buffered));

        // fill out message metadata
        new_msg->source = proc;
        new_msg->tag = meta_buf[0];
        new_msg->count = meta_buf[1];

        // add node to queue
        ASSERT_ZERO(pthread_mutex_lock(&queue.mutex));
        queue.end->prev->next = new_node;
        new_node->prev = queue.end->prev;
        queue.end->prev = new_node;
        new_node->next = queue.end;
        ASSERT_ZERO(pthread_mutex_unlock(&queue.mutex));

        if (!found_matching_msg && match(msg_pattern, new_msg)) {
            found_matching_msg = true;
            ASSERT_ZERO(pthread_cond_signal(&matched_msg));
        }

        // allocate space for the message
        ASSERT_NOT_NULL(new_msg->buffer = malloc(new_msg->count));

        if (new_msg->count == 0) {
            ASSERT_ZERO(pthread_mutex_unlock(&new_msg->is_buffered));
            continue;
        }
        
        // read the message
        void *buf_ptr = new_msg->buffer;
        int bytes_left = new_msg->count;
        while (bytes_left) {
            int read_bytes = chrecv(read_fd[proc], buf_ptr, MIN(bytes_left, MIMPI_CHANNEL_BUF));
            if (read_bytes == 0) { // pipe closed mid-write
                *result = -1;
                return result;
            }
            buf_ptr += read_bytes;
            bytes_left -= read_bytes;
        }

        // message fully buffered
        ASSERT_ZERO(pthread_mutex_unlock(&new_msg->is_buffered));
    }
}

void MIMPI_Init(bool enable_deadlock_detection) {
    channels_init();

    char *tmp;
    ASSERT_NOT_NULL(tmp = getenv(MIMPI_WORLD_VAR));
    world_size = atoi(tmp);
    
    ASSERT_NOT_NULL(tmp = getenv(MIMPI_RANK_VAR));
    my_rank = atoi(tmp);

    ASSERT_NOT_NULL(write_fd = malloc(world_size*sizeof(int)));
    ASSERT_NOT_NULL(read_fd = malloc(world_size*sizeof(int)));
    for (int i = 0; i < world_size; i++) {
        left_MIMPI_block[i] = 0;
        if (i == my_rank) {continue;}

        char *read_var = malloc(sizeof(char)*32);
        snprintf(read_var, 32, "MIMPI_READ_PIPE_%d", i);
        ASSERT_NOT_NULL(tmp = getenv(read_var));
        read_fd[i] = atoi(tmp);
        free(read_var);

        char *write_var = malloc(sizeof(char)*32);
        snprintf(write_var, 32, "MIMPI_WRITE_PIPE_%d", i);
        ASSERT_NOT_NULL(tmp = getenv(write_var));
        write_fd[i] = atoi(tmp);
        free(write_var);
    }
    
    // head and tail are dummy nodes, so that they never change
    // and we always insert/remove "in the middle"
    // which removes checking for begin/end edgecases
    

    ASSERT_NOT_NULL(queue.begin = malloc(sizeof(MIMPI_Node)));
    ASSERT_NOT_NULL(queue.end = malloc(sizeof(MIMPI_Node)));

    queue.begin->prev = queue.end->next = NULL;
    queue.begin->msg = queue.end->msg = NULL;

    queue.begin->next = queue.end;
    queue.end->prev = queue.begin;
    
    pthread_mutexattr_t mutex_attr;
    ASSERT_ZERO(pthread_mutexattr_init(&mutex_attr));
    ASSERT_ZERO(pthread_mutex_init(&queue.mutex, &mutex_attr));
    ASSERT_ZERO(pthread_mutex_init(&queue.mutex, &mutex_attr));
    ASSERT_ZERO(pthread_mutexattr_destroy(&mutex_attr));

    ASSERT_ZERO(pthread_cond_init(&matched_msg, NULL));

    pthread_attr_t attr;
    ASSERT_ZERO(pthread_attr_init(&attr));
    ASSERT_ZERO(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE));

    for (int i = 0; i < world_size; i++) {
        if (i == my_rank) {continue;}
        int *receiver_arg = malloc(sizeof(int));
        ASSERT_NOT_NULL(receiver_arg);
        *receiver_arg = i;
        ASSERT_ZERO(pthread_create(&threads[i], &attr, MIMPI_Receiver, receiver_arg));
    }

    ASSERT_ZERO(pthread_attr_destroy(&attr));
}

void MIMPI_Finalize() {
    // ping everyone else's threads that I'm leaving
    for (int i = 0; i < world_size; i++) {
        if (i == my_rank) {continue;}
        MIMPI_Send(NULL, 0, i, -7);
    }

    // custom MIMPI_barrier()

    const int l_child = (my_rank+1)*2-1, r_child = l_child+1;
    const int parent = (my_rank+1)/2-1;

    if (l_child < world_size) {
        MIMPI_Recv(NULL, 0, l_child, FINALIZE_BEGIN);
        if (r_child < world_size) {
            MIMPI_Recv(NULL, 0, r_child, FINALIZE_BEGIN);
        }
    }
    if (my_rank != 0) {
        MIMPI_Send(NULL, 0, parent, FINALIZE_BEGIN);
    }
    
    if (my_rank != 0) {
        MIMPI_Recv(NULL, 0, parent, FINALIZE_END);
    }
    if (l_child < world_size) {
        MIMPI_Send(NULL, 0, l_child, FINALIZE_END);
        if(r_child < world_size) {
            MIMPI_Send(NULL, 0, r_child, FINALIZE_END);
        }
    }

    // ping everyone else's threads to stop working
    for (int i = 0; i < world_size; i++) {
        if (i == my_rank) {continue;}
        MIMPI_Send(NULL, 0, i, -1);
    }

    for (int i = 0; i < world_size; i++) {
        if (i == my_rank) {continue;}
        int* result;
        ASSERT_ZERO(pthread_join(threads[i], (void**)&result));
        free(result);
    }

    // close pipes and unset their env vars
    for (int i = 0; i < world_size; i++) {
        if (i == my_rank) {continue;}

        char *read_var = malloc(sizeof(char)*32);
        snprintf(read_var, 32, "MIMPI_READ_PIPE_%d", i);
        ASSERT_SYS_OK(close(read_fd[i]));
        ASSERT_SYS_OK(unsetenv(read_var));
        free(read_var);
        

        char *write_var = malloc(sizeof(char)*32);
        snprintf(write_var, 32, "MIMPI_WRITE_PIPE_%d", i);
        ASSERT_SYS_OK(close(write_fd[i]));
        ASSERT_SYS_OK(unsetenv(write_var));
        free(write_var);
    }

    // unset rank and world size
    ASSERT_SYS_OK(unsetenv(MIMPI_RANK_VAR));
    ASSERT_SYS_OK(unsetenv(MIMPI_WORLD_VAR));

    free(read_fd);
    free(write_fd);

    // destroy queue
    // destroy queue mutex
    ASSERT_ZERO(pthread_mutex_destroy(&queue.mutex));
    // free queue
    MIMPI_Node *q_ptr = queue.begin;
    while (q_ptr != NULL) {
        MIMPI_Node *q_next = q_ptr->next;
        free_MIMPI_Node(q_ptr);
        q_ptr = q_next;
    }

    ASSERT_ZERO(pthread_cond_destroy(&matched_msg));

    channels_finalize();
}

int MIMPI_World_size() {
    return world_size;
}

int MIMPI_World_rank() {   
    return my_rank;
}

MIMPI_Retcode MIMPI_Send(
    void const *data,
    int count,
    int destination,
    int tag
) {
    if (my_rank == destination) 
        {return MIMPI_ERROR_ATTEMPTED_SELF_OP;}
    if (destination < 0 || destination >= world_size) 
        {return MIMPI_ERROR_NO_SUCH_RANK;}

    // first send metadata
    const int meta_size = sizeof(int)*2;
    char buffer[MIMPI_CHANNEL_BUF];// = {tag, count};
    int *buf_ptr = (int*)buffer;
    *buf_ptr = tag;
    buf_ptr = (int*)(buffer+sizeof(int));
    *buf_ptr = count;

    if (count > 0) {
        memcpy(buffer+meta_size, data, MIN(count, MIMPI_CHANNEL_BUF-meta_size));
    }

    int res = chsend(write_fd[destination], buffer, 
                     MIN(MIMPI_CHANNEL_BUF, count+meta_size));

    // pipe closed, destination process has ended ? something broke ?
    if (res == -1) {
        return MIMPI_ERROR_REMOTE_FINISHED;
    }
    
    int total_sent = res-meta_size;
    while (count - total_sent) {
        int bytes_sent = chsend(write_fd[destination], data+total_sent, 
                                MIN(MIMPI_CHANNEL_BUF, count-total_sent));
        if (bytes_sent == -1) {
            return MIMPI_ERROR_REMOTE_FINISHED;
        }
        total_sent += bytes_sent;
    }

    return MIMPI_SUCCESS;
}

MIMPI_Retcode MIMPI_Recv(
    void *data,
    int count,
    int source,
    int tag
) {
    if (my_rank == source) 
        {return MIMPI_ERROR_ATTEMPTED_SELF_OP;}
    if (source < 0 || source >= world_size) 
        {return MIMPI_ERROR_NO_SUCH_RANK;}

    found_matching_msg = true;
    MIMPI_Message *pattern = malloc(sizeof(MIMPI_Message));
    *pattern = (MIMPI_Message) {.source = source, .tag = tag, .count = count};

    // get access to queue 
    ASSERT_ZERO(pthread_mutex_lock(&queue.mutex));
    // go through queue 
    MIMPI_Node *q_ptr = queue.begin->next, *recv_node = NULL;

    while (q_ptr != queue.end) {
        if (match(pattern, q_ptr->msg)) {
            recv_node = q_ptr;
            break;
        }
        q_ptr = q_ptr->next;
    }

    // no matching message found, 
    // set what message are we looking for before anyone adds to the queue
    if (!recv_node) {

        msg_pattern = pattern;
        found_matching_msg = false;
        q_ptr = q_ptr->prev; // remember last checked node

        if (tag == GROUP_BEGIN || tag == GROUP_END) {
            while (!found_matching_msg && !left_MIMPI_block[source] && !group_failed) {
                ASSERT_ZERO(pthread_cond_wait(&matched_msg, &queue.mutex));
            }
            if (!found_matching_msg && left_MIMPI_block[source]) {
                found_matching_msg = true;
                free(pattern);
                ASSERT_ZERO(pthread_mutex_unlock(&queue.mutex));
                MIMPI_Send(NULL, 0, 0, GROUP_FAIL);
                return MIMPI_ERROR_REMOTE_FINISHED;
            }
            if (group_failed) {
                found_matching_msg = true;
                free(pattern); 
                ASSERT_ZERO(pthread_mutex_unlock(&queue.mutex));
                return MIMPI_ERROR_REMOTE_FINISHED;
            }
        }
        else {
            while (!found_matching_msg && (tag<0 || !left_MIMPI_block[source])) {
                ASSERT_ZERO(pthread_cond_wait(&matched_msg, &queue.mutex));
            }
        }

        if (!found_matching_msg) {
            found_matching_msg = true;
            free(pattern); 
            ASSERT_ZERO(pthread_mutex_unlock(&queue.mutex));
            return MIMPI_ERROR_REMOTE_FINISHED;
        }

        // iterate through the rest of the queue until we find the matching one
        q_ptr = q_ptr->next;
        while (q_ptr != queue.end) {
            if (match(pattern, q_ptr->msg)) {
                recv_node = q_ptr;
                break;
            }
            q_ptr = q_ptr->next;
        }
    }
    ASSERT_ZERO(pthread_mutex_unlock(&queue.mutex));

    // wait until the data is fully buffered
    ASSERT_ZERO(pthread_mutex_lock(&recv_node->msg->is_buffered));
    // move the data 
    if (recv_node->msg->count > 0) {
        memcpy(data, recv_node->msg->buffer, recv_node->msg->count);
    }
    ASSERT_ZERO(pthread_mutex_unlock(&recv_node->msg->is_buffered));

    // remove node from queue
    
    ASSERT_ZERO(pthread_mutex_lock(&queue.mutex));
    recv_node->prev->next = recv_node->next;
    recv_node->next->prev = recv_node->prev;
    ASSERT_ZERO(pthread_mutex_unlock(&queue.mutex));
 
    // free it
    free_MIMPI_Node(recv_node);
    found_matching_msg = true;
    free(pattern); // no longer needed

    return MIMPI_SUCCESS;
}

MIMPI_Retcode MIMPI_Barrier() {
    const int l_child = (my_rank+1)*2-1, r_child = l_child+1;
    const int parent = (my_rank+1)/2-1;

    if (l_child < world_size) {
        ASSERT_MIMPI_RECV_OK(MIMPI_Recv(NULL, 0, l_child, GROUP_BEGIN));
        if (r_child < world_size) {
            ASSERT_MIMPI_RECV_OK(MIMPI_Recv(NULL, 0, r_child, GROUP_BEGIN));
        }
    }
    if (my_rank != 0) {
        MIMPI_Send(NULL, 0, parent, GROUP_BEGIN);
    }
    
    if (my_rank != 0) {
        ASSERT_MIMPI_RECV_OK(MIMPI_Recv(NULL, 0, parent, GROUP_END));
    }
    if (l_child < world_size) {
        MIMPI_Send(NULL, 0, l_child, GROUP_END);
        if(r_child < world_size) {
            MIMPI_Send(NULL, 0, r_child, GROUP_END);
        }
    }

    return MIMPI_SUCCESS;
}

inline static int MIMPI_real_proc(int rank, int root) {
    if (rank == root)
        return 0;
    if (rank == 0) 
        return root;
    return rank;
}

MIMPI_Retcode MIMPI_Bcast(
    void *data,
    int count,
    int root
) {
    const int treat_as = MIMPI_real_proc(my_rank, root);
    const int l_child = MIMPI_real_proc((treat_as+1)*2-1, root);
    const int r_child = MIMPI_real_proc((treat_as+1)*2, root);
    const int parent = MIMPI_real_proc((treat_as+1)/2-1, root);

    if (l_child < world_size) {
        ASSERT_MIMPI_RECV_OK(MIMPI_Recv(NULL, 0, l_child, GROUP_BEGIN));
        if (r_child < world_size) {
            ASSERT_MIMPI_RECV_OK(MIMPI_Recv(NULL, 0, r_child, GROUP_BEGIN));
        }
    }
    if (treat_as != 0) {
        MIMPI_Send(NULL, 0, parent, GROUP_BEGIN);
    }

    if (treat_as != 0) {
        ASSERT_MIMPI_RECV_OK(MIMPI_Recv(data, count, parent, GROUP_END));
    }
    if (l_child < world_size) {
        MIMPI_Send(data, count, l_child, GROUP_END);
        if(r_child < world_size) {
            MIMPI_Send(data, count, r_child, GROUP_END);
        }
    }
    
    return MIMPI_SUCCESS;
}

inline static void reduce_data(char *dest, char *src, int count, const MIMPI_Op op) {
    for (int i = 0; i < count; i++) {
        if (op == MIMPI_MAX) 
            dest[i] = MAX(dest[i], src[i]);
        else if (op == MIMPI_MIN) 
            dest[i] = MIN(dest[i], src[i]);
        else if (op == MIMPI_SUM)
            dest[i] += src[i];
        else 
            dest[i] *= src[i];
    }
    return;
}

MIMPI_Retcode MIMPI_Reduce(
    void const *send_data,
    void *recv_data,
    int count,
    MIMPI_Op op,
    int root
) {
    const int treat_as = MIMPI_real_proc(my_rank, root);
    const int l_child = MIMPI_real_proc((treat_as+1)*2-1, root);
    const int r_child = MIMPI_real_proc((treat_as+1)*2, root);
    const int parent = MIMPI_real_proc((treat_as+1)/2-1, root);

    char *reduced_data;
    if (treat_as == 0) {
        reduced_data = recv_data;
    } else {
        ASSERT_NOT_NULL(reduced_data = malloc(count));
    }
    ;
    memcpy(reduced_data, send_data, count);

    if (l_child < world_size) {
        char *tmp_buf = malloc(count);
        ASSERT_MIMPI_RECV_OK(MIMPI_Recv(tmp_buf, count, l_child, GROUP_BEGIN));
        reduce_data(reduced_data, tmp_buf, count, op);
        if (r_child < world_size) {
            ASSERT_MIMPI_RECV_OK(MIMPI_Recv(tmp_buf, count, r_child, GROUP_BEGIN));
            reduce_data(reduced_data, tmp_buf, count, op);
        }
        free(tmp_buf);
    }

    if (treat_as != 0) {
        MIMPI_Send(reduced_data, count, parent, GROUP_BEGIN);
        free(reduced_data);
        ASSERT_MIMPI_RECV_OK(MIMPI_Recv(NULL, 0, parent, GROUP_END));
    }

    if (l_child < world_size) {
        MIMPI_Send(NULL, 0, l_child, GROUP_END);
        if(r_child < world_size) {
            MIMPI_Send(NULL, 0, r_child, GROUP_END);
        }
    }

    return MIMPI_SUCCESS;
}