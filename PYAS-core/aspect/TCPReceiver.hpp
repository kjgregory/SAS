#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), connect(), sendto(), and recvfrom() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_addr() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_addr() */
#include "Command.hpp"
#include "Telemetry.hpp"

#define PACKET_MAX_SIZE 300

class TCPReceiver {
    protected:
        int sender_sock;                /* Socket of someone connecting to me */
        int my_sock;                /* My Socket */
        struct sockaddr_in myAddr;      /* Local address */
        struct sockaddr_in senderAddr;  /* Sender address */
        socklen_t senderAddrLen;

        char payload[PACKET_MAX_SIZE];  /* Buffer for echo string */
        unsigned short listeningPort;   /* The port to listen to */
        unsigned int numBytesRcvd;                /* Size of received message */

    public:
        TCPReceiver( void );
        TCPReceiver( unsigned short port );
        
        void get_packet( uint8_t *packet  );
        unsigned int handle_tcpclient( int client_socket );
        int accept_packet();
        void init_connection( void );
        void close_connection( void );
};