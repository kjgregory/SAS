#include "Command.hpp"
#include "Telemetry.hpp"

class TCPSender {
    protected:
        int sock;                       /* Socket descriptor */
        struct sockaddr_in sendAddr;    /* Echo server address */
        unsigned int fromSize;          /* In-out of address size for recvfrom() */
        char *sendtoIP;                 /* IP address to send to */
        unsigned short sendPort;        /* Port to send on*/

    public:
        TCPSender( void );
        TCPSender( const char *ip, unsigned short port );
	    ~TCPSender();
        
        virtual void send_packet(  TelemetryPacket *packet  );
        int init_connection( void );
        void close_connection( void );
};
