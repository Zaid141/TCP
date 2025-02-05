#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD 0

int RETRY = 3000; // timeout timer value (RTO)

// 0 to 4,294,967,295
u_int32_t next_seqno = 0;
u_int32_t send_base = 0;

int WINDOW_SIZE = 1; // cwnd size
double wsize = 1; //used for congestion avoidance incrementation
int arrsize = 1; //the size of the array that holds the window

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet **window; //CWND where window[0] serves as 'base' for oldest unacked packet

sigset_t sigmask;
double estRTT = 0;
int timeoutseqnum = 0; // seqnum for which the timeout happened

int timer_running = 0; //flag to indicate whether the timer is running or not
int eof = 0;           //flag to indicate whether the end is reached or not

int ssthresh = 64;

FILE *fp;
FILE *logcsv;

struct packet_info
{
    struct timeval send_time;
    int retransmitted;
};

struct node
{
    int seqno;
    struct timeval send_time;
    int retransmitted;
    struct node *next;
};

struct node *head = NULL;  //linked list which stores timestamp and retransmitted status of packets

// inserts new node into linked list
void insert(int seqno, struct timeval send_time, int retransmitted)
{
    struct node *new_node = (struct node *)malloc(sizeof(struct node));
    if (new_node == NULL)
    {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    new_node->seqno = seqno;
    new_node->send_time = send_time;
    new_node->retransmitted = retransmitted;
    new_node->next = head;
    head = new_node;
}

// Function to find a node in the linked list with a specific seqno and return it
struct node *find(int seqno)
{
    struct node *current = head;
    while (current != NULL)
    {
        if (current->seqno == seqno)
        {
            return current;
        }
        current = current->next;
    }
    return NULL; //node not found
}

int window_empty() // checks whether the whole window is empty or not
{
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        if (window[i] != NULL)
        {
            return 0;
        }
    }

    return 1;
}

// struct packet_info send_times[WINDOW_SIZE];
void init_timer(int delay, void (*sig_handler)(int))
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000; // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000; // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

void start_timer()
{
    if (!timer_running) // Start timer only if it isn't already running
    {
        sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
        setitimer(ITIMER_REAL, &timer, NULL);
        timer_running = 1;
    }
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
    timer_running = 0;
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        // TODO: modify ssthresh, set cwnd=1

        VLOG(INFO, "Timout happend");

        if ((WINDOW_SIZE / 2) > 2)
        {
            ssthresh = WINDOW_SIZE / 2;
        }
        else
        {
            ssthresh = 2;
        }

        WINDOW_SIZE = 1;
        wsize = 1;

        struct timeval tv;
        gettimeofday(&tv, NULL); // get current time with milliseconds
        time_t seconds = tv.tv_sec;
        long microseconds = tv.tv_usec;
        fprintf(logcsv, "%ld.%06ld,%f,%d\n", seconds, microseconds, wsize, ssthresh); // for CWND.csv

        if (window[0] != NULL)
        {

            if (sendto(sockfd, window[0], TCP_HDR_SIZE + get_data_size(window[0]), 0, // replace sendpkt with window[0]
                       (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            find(window[0]->hdr.seqno)->retransmitted = 1; 

            // exponential backoff
            if (timeoutseqnum == window[0]->hdr.seqno)
            {
                if ((2 * RETRY) < 240000) // need to change 8000 to 240000
                {
                    RETRY = 2 * RETRY;
                }
                else
                {
                    RETRY = 240000; // need to change 8000 to 240000
                }

                init_timer(RETRY, resend_packets);
            }

            timeoutseqnum = window[0]->hdr.seqno; // so that if the timeout repeats for certain packet exponential backoff can be used
        }
        else if (eof && window_empty()) // end of file on sender side and all packets have been acked/recevied at receiver end (termination condition)
        {
            free(sndpkt);
            sndpkt = make_packet(0);
            window[0] = sndpkt;
            int resendcount = 0;

            // ensures receiver gets the final packet
            while (resendcount < 100)
            {
                sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                       (const struct sockaddr *)&serveraddr, serverlen);
                resendcount++;
            }
            
            free(sndpkt);
            fclose(fp);
            fclose(logcsv);
            exit(0);
        }

        timer_running = 0;
        stop_timer();
    }
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */

int main(int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];

    /* check command line arguments */
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL)
    {
        error(argv[3]);
    }

    logcsv = fopen("CWND.csv", "w");
    if (logcsv == NULL)
    {
        error("log");
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0)
    {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    init_timer(RETRY, resend_packets);

    next_seqno = 0;

    window = (tcp_packet **)malloc(WINDOW_SIZE * sizeof(tcp_packet *)); // dynamic allocation
    if (window == NULL)
    {
        fprintf(stderr, "Memory allocation failed\n");
        return 1; // Exit with error code
    }

    int dupack = 0; // counts the number of dupacks for lowest sequence number (window[0])
    while (1)
    {

        while (window[WINDOW_SIZE - 1] != NULL || (eof)) // either while window is full, or if have reached eof on sender side and the window is still not empty (used to be || (eof && !window_empty()))
        {
            // Wait until the first packet in the window is acknowledged
            if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                         (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
            {
                error("recvfrom");
            }

            recvpkt = (tcp_packet *)buffer;
            printf("%d \n", get_data_size(recvpkt));

            if (window[0] != NULL) // to avoid segfault when doing window[0]->hdr.seqno
            {
                if (recvpkt->hdr.ackno > window[0]->hdr.seqno) // as long as ack is greater than window[0] seqnum, then window[0] has definitely been received
                {
                    stop_timer(); // stop timer as oldest packet has defintely been received

                    if (!find(window[0]->hdr.seqno)->retransmitted) // checks if it is a retransmitted packet
                    {
                        struct timeval now;
                        gettimeofday(&now, NULL);
                        double rtt = (now.tv_sec - find(window[0]->hdr.seqno)->send_time.tv_sec) * 1000.0 + (now.tv_usec - find(window[0]->hdr.seqno)->send_time.tv_usec) / 1000.0;
                        estRTT = 0.875 * estRTT + 0.125 * rtt;
                        RETRY = 2 * estRTT;
                        init_timer(RETRY, resend_packets);
                        
                    }

                    // slide window
                    while (window[0] != NULL && recvpkt->hdr.ackno > window[0]->hdr.seqno) // essentially slide window until first element in window is greater or equal to the ack received
                    {

                        // slow start (increase window by 1 every ack)
                        if (WINDOW_SIZE < ssthresh)
                        {
                            // dynamic allocation
                            if (WINDOW_SIZE == arrsize)
                            {
                                tcp_packet **temp = (tcp_packet **)realloc(window, (arrsize * 2) * sizeof(tcp_packet *));
                                if (temp == NULL)
                                {
                                    fprintf(stderr, "Memory reallocation failed\n");
                                    free(window); //free the previously allocated memory
                                    return 1;     //exit with error code
                                }
                                window = temp;
                                
                                for (int i = arrsize; i < arrsize*2; i++)
                                {
                                    window[i] = NULL;
                                }

                                arrsize = arrsize * 2;
                            }

                            WINDOW_SIZE++;
                            wsize++;
                        }
                        else // congestion avoidance
                        {
                            wsize = wsize + (1 / (double)WINDOW_SIZE); // will only increment by a whole number when whole window is ACKED
                            if (((int)wsize - WINDOW_SIZE) == 1)       // if WINDOW_SIZE should be updated or not
                            {
                                // dynamic allocation
                                if (WINDOW_SIZE == arrsize)
                                {
                                    tcp_packet **temp = (tcp_packet **)realloc(window, (arrsize * 2) * sizeof(tcp_packet *));
                                    if (temp == NULL)
                                    {
                                        fprintf(stderr, "Memory reallocation failed\n");
                                        free(window); //free the previously allocated memory
                                        return 1;     //exit with error code
                                    }
                                    window = temp;
                                    
                                    for (int i = arrsize; i < arrsize*2; i++)
                                    {
                                        window[i] = NULL;
                                    }
                                    arrsize = arrsize * 2;
                                }

                                WINDOW_SIZE++;
                            }
                        }

                        struct timeval tv;
                        gettimeofday(&tv, NULL); // get current time with milliseconds
                        time_t seconds = tv.tv_sec;
                        long microseconds = tv.tv_usec;
                        fprintf(logcsv, "%ld.%06ld,%f,%d\n", seconds, microseconds, wsize, ssthresh); // for CWND.csv
                        

                        for (int i = 0; i < arrsize - 1; i++) // dynamic allocation
                        {
                            if (window[i] == NULL) // so that loop doesnt run for arrsize-1 times and only shifts NON-NULL elements
                            {
                                break;
                            }

                            window[i] = window[i + 1];
                        }

                    }

                    if (eof && window_empty()) // end of file on sender side and all packets have been acked/recevied at receiver end (termination condition)
                    {
                        
                        free(sndpkt);
                        sndpkt = make_packet(0);
                        window[0] = sndpkt;

                        int resendcount = 0;

                        // ensures receiver gets the final packet
                        while (resendcount < 100)
                        {
                            sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                                   (const struct sockaddr *)&serveraddr, serverlen);
                            resendcount++;
                        }

                        free(sndpkt);
                        fclose(fp);
                        fclose(logcsv);
                        return 0;
                    }

                    start_timer(); // for next packet
                }
                else if (recvpkt->hdr.ackno == window[0]->hdr.seqno) // receiver is asking for oldest packet in window
                {
                    dupack++;
                    
                    if (dupack == 3) // packet is lost and must do a fast retransmit
                    {
                        if (WINDOW_SIZE < ssthresh) // slow start
                        {
                            if ((WINDOW_SIZE / 2) > 2)
                            {
                                ssthresh = WINDOW_SIZE / 2;
                            }
                            else
                            {
                                ssthresh = 2;
                            }
                        }
                        else // congestion avoidance
                        {
                            if ((WINDOW_SIZE / 2) > 2)
                            {
                                ssthresh = WINDOW_SIZE / 2;
                            }
                            else
                            {
                                ssthresh = 2;
                            }

                            WINDOW_SIZE = 1;
                            wsize = 1;
                        }

                        struct timeval tv;
                        gettimeofday(&tv, NULL); // get current time with milliseconds
                        time_t seconds = tv.tv_sec;
                        long microseconds = tv.tv_usec;
                        fprintf(logcsv, "%ld.%06ld,%f,%d\n", seconds, microseconds, wsize, ssthresh); // for CWND.csv

                        if (sendto(sockfd, window[0], TCP_HDR_SIZE + get_data_size(window[0]), 0,
                                   (const struct sockaddr *)&serveraddr, serverlen) < 0)
                        {
                            error("sendto");
                        }

                        
                        find(window[0]->hdr.seqno)->retransmitted = 1;
                        

                        dupack = 0;
                    }
                }
            }
            else if (eof && window_empty()) // end of file on sender side and all packets have been acked/recevied at receiver end (termination condition)
            {
                free(sndpkt);
                sndpkt = make_packet(0);
                window[0] = sndpkt;
                int resendcount = 0;

                // ensures receiver gets the final packet
                while (resendcount < 100)
                {
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (const struct sockaddr *)&serveraddr, serverlen);
                    resendcount++;
                }

                
                free(sndpkt);
                fclose(fp);
                fclose(logcsv);
                return 0;
            }
        }

        while (window[WINDOW_SIZE - 1] == NULL) // read and send data until window is full
        {
            len = fread(buffer, 1, DATA_SIZE, fp); // length of one packet
            if (len <= 0)
            {
                VLOG(INFO, "End Of File has been reached sender side");

                eof = 1;
                break;
            }
            send_base = next_seqno;
            next_seqno = send_base + len; // will wrap around 0-4294967295
            sndpkt = make_packet(len);
            
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = send_base;
            for (int i = 0; i < WINDOW_SIZE; i++)
            {
                if (window[i] == NULL)
                {
                    window[i] = sndpkt; // puts the new packet in the first free spot

                    break;
                }
            }

            VLOG(DEBUG, "Sending packet %d to %s",
                 send_base, inet_ntoa(serveraddr.sin_addr));
            /*
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                       (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            struct timeval now;
            gettimeofday(&now, NULL);
            insert(sndpkt->hdr.seqno, now, 0);
            start_timer(); // would start timer only if timer isn't already running
        }
    }

    return 0;
}
