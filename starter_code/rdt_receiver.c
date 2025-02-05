#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>
#include <time.h>

#include "common.h"
#include "packet.h"

#define WINDOW_SIZE 10 // Define the size of the sliding window for packet buffering

typedef struct
{
    tcp_packet *pkt; // Pointer to a packet structure
    int valid;       // Flag to check if the buffer slot is occupied
} PacketBuffer;

PacketBuffer window[WINDOW_SIZE]; // Sliding window buffer
int next_seqno = 0;               // Expected sequence number of the next packet
int base_seqno = 0;               // Sequence number of the first packet in the sliding window

void buffer_packet(tcp_packet *pkt)
{
    int index = (pkt->hdr.seqno - base_seqno) / DATA_SIZE;
    if (index < 0 || index >= WINDOW_SIZE)
        return;

    if (!window[index].valid)
    {
        //printf("Buffering packet %d at index %d\n", pkt->hdr.seqno, index); //debugging
        window[index].pkt = malloc(sizeof(tcp_packet) + pkt->hdr.data_size);
        memcpy(window[index].pkt, pkt, sizeof(tcp_packet) + pkt->hdr.data_size);
        window[index].valid = 1;
    }
}

void write_buffered_packets(FILE *fp) {
    // Adjust base_seqno if necessary
    while (!window[0].valid && base_seqno < next_seqno) {
        base_seqno += DATA_SIZE; // Adjust base sequence number
        memmove(window, window + 1, (WINDOW_SIZE - 1) * sizeof(PacketBuffer)); // Slide window
        memset(&window[WINDOW_SIZE - 1], 0, sizeof(PacketBuffer)); // Clear the last slot
    }

    // Write packets starting from the first valid one that matches next_seqno
    int i = 0;
    while (i < WINDOW_SIZE && window[i].valid && window[i].pkt->hdr.seqno == next_seqno) {
        //printf("Writing buffered packet %d\n", window[i].pkt->hdr.seqno); //debugging
        fwrite(window[i].pkt->data, 1, window[i].pkt->hdr.data_size, fp);
        next_seqno += window[i].pkt->hdr.data_size;

        free(window[i].pkt);
        window[i].pkt = NULL;
        window[i].valid = 0;
        i++;
    }
    // Slide window to remove written packets
    if (i > 0) {
        memmove(window, window + i, (WINDOW_SIZE - i) * sizeof(PacketBuffer));
        memset(window + (WINDOW_SIZE - i), 0, i * sizeof(PacketBuffer));
    }
}


int main(int argc, char **argv)
{
    int sockfd;                                // Socket file descriptor
    int portno;                                // Port number
    socklen_t clientlen;                       // Byte size of client's address
    struct sockaddr_in serveraddr, clientaddr; // Server and client address structures
    int optval;                                // Option value for setsockopt
    FILE *fp;                                  // File pointer for the received file
    char buffer[MSS_SIZE];                     // Buffer to receive data

    // Check for correct command-line arguments
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <port> <FILE_RECVD>\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);    // Convert the port number from string to integer
    fp = fopen(argv[2], "wb"); // Open the output file in write-binary mode
    if (fp == NULL)
    {
        error("Error opening file");
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0); // Create a UDP socket
    if (sockfd < 0)
        error("ERROR opening socket");

    // Allow reuse of local addresses, for rapid restarts of the server
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));

    // Initialize the server address structure
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    // Bind the socket to the port
    if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
        error("ERROR on binding");

    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    // Initialize the sliding window
    memset(window, 0, sizeof(window));

    // Main loop: wait for a datagram, then process it
    while (1)
    {
        // Receive data from the client
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&clientaddr, &clientlen) < 0)
        {
            error("ERROR in recvfrom");
        }

        // Process the received packet
        tcp_packet *recvpkt = (tcp_packet *)buffer;
        if (recvpkt->hdr.data_size == 0)
        { // Check for end-of-file packet
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            tcp_packet *sndpkt = make_packet(0);
            sndpkt->hdr.ackno = next_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            for (int i = 0; i < 100; i++)
            { // Send an ACK for the EOF packet multiple times
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (struct sockaddr *)&clientaddr, clientlen) < 0)
                {
                    error("ERROR in sendto");
                }
            }
            break; // Exit the main loop
        }

        //printf("Current next_seqno: %d\n", next_seqno);
        //printf("Packet seqno: %d, data size: %d\n", recvpkt->hdr.seqno, recvpkt->hdr.data_size);

        // Write packet directly to file if it is the next expected packet
        if (recvpkt->hdr.seqno == next_seqno)
        {
            time_t now = time(NULL); // Get the current epoch time
            printf("%ld, %d, %d\n", now, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            //printf("Packet written directly, updated next_seqno to: %d\n", next_seqno); //debugging
            next_seqno += recvpkt->hdr.data_size;
            //printf("Updated next_seqno to %d after writing packet with seqno %d\n", next_seqno, recvpkt->hdr.seqno); //debugging
            write_buffered_packets(fp);
        }
        else if (recvpkt->hdr.seqno > next_seqno) 
        {
            buffer_packet(recvpkt);
            //printf("Packet buffered, next_seqno remains: %d\n", next_seqno); //debugging
        }
        //printf("Post-update next_seqno: %d\n", next_seqno); //debugging

        // Always send an ACK for the highest consecutive packet received
        tcp_packet *sndpkt = make_packet(0);
        sndpkt->hdr.ackno = next_seqno;
        sndpkt->hdr.ctr_flags = ACK;
        sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&clientaddr, clientlen);
    }

    close(sockfd); // Close the socket
    return 0;
}