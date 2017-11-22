/*

Adjustments which need to take place so that the packets, and sessions cannot easily be filtered.  HTTP modifications are done within
http.c because it will likely rely on other functions.

*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include "network.h"
#include "antisurveillance.h"
#include "packetbuilding.h"
#include "http.h"



// We wouldn't want the surveillance platforms to see the same exact packets.. over and over..
// Let's adjust the source port, identifiers, ACK/SEQ bases, etc here..
// This function will have to call other functions soon to modify MACROS. (dynamic portions of the packets
// which are intended to show other differences..) It could even load other messages in some cases.
// it depends on how your attacks are targeted.
// Adjustments needs to be modular in a way to easily support IPv4/6 and ICMP/UDP/TCP on both stacks.
void PacketAdjustments(AS_attacks *aptr) {
    // our new source port must be above 1024 and below 65536 (generally acceptable on all OS) <1024 is privileged
    int client_port = (1024 + rand()%(65535 - 1024));
    // the identifier portion of the packets..
    int client_identifier = rand()%0xFFFFFFFF;
    int server_identifier = rand()%0xFFFFFFFF;

    // our new SEQ base for each side of the connection.. it will replace all packets current sequence numbers
    int client_new_seq = rand()%0xFFFFFFFF;
    int server_new_seq = rand()%0xFFFFFFFF;

    // it picks new  IPs randomly right now.. it needs a context for the current configuration for picking them (w historic information)
    uint32_t src_ip = inet_addr("10.0.0.1");//rand()%0xFFFFFFFF;
    uint32_t dst_ip = inet_addr("10.0.0.4");//rand()%0xFFFFFFFF;

    // used to change the SEQ.. it calculates the difference between the old, and new so that the rest is compatible
    uint32_t client_seq_diff = 0;
    uint32_t server_seq_diff = 0;

    // if this attack doesn't wanna get adjusted...
    if (aptr->skip_adjustments) {
        BuildPackets(aptr);
        return;
    }

    // loop through each packet instruction in memory for this attack
    PacketBuildInstructions *buildptr = aptr->packet_build_instructions;
    while (buildptr != NULL) {
        // we can determine which side of the connection by this variable we had set during analysis
        if (buildptr->client) {
            // set our new IP addresses for this packet
            buildptr->source_ip = src_ip;
            buildptr->destination_ip = dst_ip;

            // Source port from client side to server is changed here
            buildptr->source_port = client_port;
            // The header identifier is changed here (and we are using the client side)
            buildptr->header_identifier = client_identifier++;

            // replace tcp/ip ack/seq w new base
            client_seq_diff = buildptr->seq - aptr->client_base_seq;
            buildptr->seq = client_new_seq + client_seq_diff;

            // we do not wish to replace an ACK of 0.. (the initial syn packet)
            if (buildptr->ack != 0) {
                server_seq_diff = buildptr->ack - aptr->server_base_seq;
                buildptr->ack = server_new_seq + server_seq_diff;
            }

            aptr->destination_port = buildptr->destination_port;
            aptr->source_port = buildptr->source_port;
        } else  {
            // set it using opposite information for a packet from the server side
            buildptr->source_ip = dst_ip;
            buildptr->destination_ip = src_ip;
            
            // Source port from server to client is changed here
            buildptr->destination_port = client_port;
            // The header identifier is changed here (and we use the server side)
            buildptr->header_identifier = server_identifier++;

            // replace w our new base seqs for server side packets.. (and if we are ACK'ing client packet)
            server_seq_diff = buildptr->seq - aptr->server_base_seq;
            buildptr->seq = server_new_seq + server_seq_diff;

            if (buildptr->ack != 0) {
                client_seq_diff = buildptr->ack - aptr->client_base_seq;
                buildptr->ack = client_new_seq + client_seq_diff;
            }
        }

        
        // move to the next packet
        buildptr = buildptr->next;
    }

    // make sure we update the attack structure for next ACK/SEQ adjustment
    aptr->client_base_seq = client_new_seq;
    aptr->server_base_seq = server_new_seq;

    // We would like to manipulate HTTP sessions to increase resources, etc on mass surveillance platforms.
    if (aptr->destination_port == 80) HTTPContentModification(aptr);

    // Rebuild all packets using the modified instructions
    BuildPackets(aptr);

    return;
}


//int BuildHTTP4Session(AS_attacks *aptr, uint32_t server_ip, uint32_t client_ip, uint32_t server_port,  char *client_body,
//int client_size, char *server_body, int server_size);

//IP stragies for PacketAadjustments..

typedef struct _ip_strategy {
	struct _ip_strategy *next;

	int ts;
	
	// so multiple strategies can get used...
	int last_use;
	// country (0 if no country)
	int geo;
	
	int ip_count;

	uint32_t **ip_list;

} IPStrategy;

	
