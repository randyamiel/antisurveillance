/*

This file contains all functionality for turning this frameworks 'Instruction' structures into final packets which are
wire ready.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <sys/time.h>
#include <pthread.h>
#include <netinet/ether.h>
#include <net/ethernet.h>

#include "network.h"
#include "antisurveillance.h"
#include "attacks.h"
#include "packetbuilding.h"
#include "adjust.h"
#include "utils.h"
#include "pcap.h"

// calculate checksum
unsigned short in_cksum(unsigned short *addr,int len) {
	register int sum = 0;
	u_short answer = 0;
	register u_short *w = addr;
	register int nleft = len;

        /*!
	* Our algorithm is simple, using a 32 bit accumulator (sum), we add
	* sequential 16 bit words to it, and at the end, fold back all the
	* carry bits from the top 16 bits into the lower 16 bits.
	*/
	while (nleft > 1)  {
		sum += *w++;
		nleft -= 2;
	}

	/*! mop up an odd byte, if necessary */
	if (nleft == 1) {
		*(u_char *)(&answer) = *(u_char *)w ;
		sum += answer;
	}

	/*! add back carry outs from top 16 bits to low 16 bits */
	sum = (sum >> 16) + (sum & 0xffff);     /*! add hi 16 to low 16 */
	sum += (sum >> 16);                     /*! add carry */
	answer = ~sum;                          /*! truncate to 16 bits */
	return(answer);
}





/* took some packet forging stuff I found online, and modified it...
   It was better than my wireshark -> C array dumping w memcpy... trying to hack this together as quickly as possible isnt fun :)

   "Generate TCP packets by jve Dated sept 2008"
*/

// Takes build instructions from things like HTTP Session generation, and creates the final network ready
// data buffers which will flow across the Internet
int BuildSingleTCP4Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    int TCPHSIZE = 20;

    //printf("build tcp4\n");

    if (PacketTCPBuildOptions(iptr) != 1) {
        //printf("failed build options\n");
        return -1;
    }

    // this is only for ipv4 tcp
    if (!(iptr->type & PACKET_TYPE_TCP_4)) {
        //printf("wrong type\n");
        return ret;
    }

    // increase the heaader by the size of the TCP options
    if (iptr->options_size) TCPHSIZE += iptr->options_size;

    // calculate full length of packet.. before we allocate memory for storage
    int final_packet_size = sizeof(struct iphdr) + TCPHSIZE + iptr->data_size;
    unsigned char *final_packet = (unsigned char *)calloc(1, final_packet_size);

    // ensure the final packet was allocated correctly
    if (final_packet == NULL) {
        //printf("couldnt alloc packet\n");
        return ret;
    }

    
    struct packet *p = (struct packet *)final_packet;
    
    // IP header below
    p->ip.version 	= 4;
    p->ip.ihl   	= sizeof(struct iphdr) >> 2;
    p->ip.tos   	= 0;    
    p->ip.frag_off 	= 0x0040;
    p->ip.protocol 	= IPPROTO_TCP;

    // Source, and destination IP addresses
    p->ip.saddr 	= iptr->source_ip;
    p->ip.daddr 	= iptr->destination_ip;

    // These two relate to dynamically changing information.. TTL=OS emulation, header identifier gets incremented..
    // and should be changed every connection that is wrote to the wire
    p->ip.id 	    = htons(iptr->header_identifier);
    p->ip.ttl 	    = iptr->ttl;

    // total length
    p->ip.tot_len   = htons(final_packet_size);
    

    // TCP header below
    // The source, and destination ports in question
    p->tcp.source   = htons(iptr->source_port);
    p->tcp.dest     = htons(iptr->destination_port);

    // The ACK/SEQ relate to variables incremented during normal communications..
    p->tcp.seq      = htonl(iptr->seq);
    p->tcp.ack_seq	= htonl((uint32_t)iptr->ack);
    /*CLR_RED();
    printf("packet seq id %04X %08X ack %08X addr: seq/ack %p %p final %p %p final %p\n", 
    ntohs(p->ip.id),
    iptr->seq, iptr->ack, &iptr->seq, &iptr->ack,
    &p->tcp.seq, &p->tcp.ack_seq, final_packet);
    CLR_RESET();*/
    // The TCP window relates to operating system emulation
    p->tcp.window	= htons(iptr->tcp_window_size);

    //printf("tcp window: %d\n", iptr->tcp_window_size);
    
    // syn/ack used the most
    p->tcp.syn  	= (iptr->flags & TCP_FLAG_SYN) ? 1 : 0;
    p->tcp.ack	    = (iptr->flags & TCP_FLAG_ACK) ? 1 : 0;
    p->tcp.rst	    = (iptr->flags & TCP_FLAG_RST) ? 1 : 0;
    p->tcp.psh  	= (iptr->flags & TCP_FLAG_PSH) ? 1 : 0;
    p->tcp.fin  	= (iptr->flags & TCP_FLAG_FIN) ? 1 : 0;

    
    p->tcp.check	= 0;	/*! set to 0 for later computing */
    p->tcp.urg	    = 0;    
    p->tcp.urg_ptr	= 0;
    p->tcp.doff 	= TCPHSIZE >> 2;

    // IP header checksum
    p->ip.check	    = (unsigned short)in_cksum((unsigned short *)&p->ip, sizeof(struct iphdr));


    /*
    // Lets make sure we cover all header variables.. just to be sure it wont be computationally possible
    // to filter out all packets....
    __u16 	res1:4 // supposed to be unused..
    __u16 	ece:1
    __u16 	cwr:1
    https://stackoverflow.com/questions/1480548/tcp-flags-present-in-the-header
    The two flags, 'CWR' and 'ECE' are for Explicit Congestion Notification as defined in RFC 3168.
    */

    // TCP header checksum
    if (p->tcp.check == 0) {
        struct pseudo_tcp4 *p_tcp = NULL;
        char *checkbuf = (char *)calloc(1,sizeof(struct pseudo_tcp4) + TCPHSIZE + iptr->data_size);

        if (checkbuf == NULL) {
            //printf("couldnt alloc for checkbuf\n");
            return -1;
        }

        p_tcp = (struct pseudo_tcp4 *)checkbuf;

        p_tcp->saddr 	= p->ip.saddr;
        p_tcp->daddr 	= p->ip.daddr;
        p_tcp->mbz      = 0;
        p_tcp->ptcl 	= IPPROTO_TCP;
        p_tcp->tcpl 	= htons(TCPHSIZE + iptr->data_size);

        memcpy(&p_tcp->tcp, &p->tcp, TCPHSIZE);
        memcpy(checkbuf + sizeof(struct pseudo_tcp4), iptr->options, iptr->options_size);
        memcpy(checkbuf + sizeof(struct pseudo_tcp4) + iptr->options_size, iptr->data, iptr->data_size);        

        // put the checksum into the correct location inside of the header
        // options size was already calculated into TCPHSIZE
        p->tcp.check = (unsigned short)in_cksum((unsigned short *)checkbuf, TCPHSIZE + sizeof(struct pseudo_tcp4) + iptr->data_size);
    
        free(checkbuf);
    }

    

    // copy the TCP options to the final packet
    if (iptr->options_size)
        memcpy(final_packet + sizeof(struct packet), iptr->options, iptr->options_size);

    // copy the data to the final packet
    if (iptr->data_size) {
        //md5hash(iptr->data, iptr->data_size);
        memcpy(final_packet + sizeof(struct packet) + iptr->options_size, iptr->data, iptr->data_size);
    }
    

    // put the final packet into the build instruction structure as completed..
    iptr->packet = (char *)final_packet;
    iptr->packet_size = final_packet_size;

    //printf("ret =1\n", ret);
    // returning 1 here will mark it as GOOD
    return (ret = 1);
}


int BuildPacketInstructions(PacketBuildInstructions *ptr) {
    PacketInfo *qptr = NULL;
    int i = 0;
    int n = 0;
    int ret = 1;

    // Structure containing each packet type, and their functions for building
    // The type must diretly correlate to the  enum {} list.. in order
    struct _packet_builders {
        int type;
        int (*func)(PacketBuildInstructions *);
    } PacketBuilders[] = {
        { PACKET_TYPE_TCP_4,    &BuildSingleTCP4Packet },
        { PACKET_TYPE_UDP_4,    &BuildSingleUDP4Packet },
        { PACKET_TYPE_ICMP_4,   &BuildSingleICMP4Packet },
        { PACKET_TYPE_TCP_6,    &BuildSingleTCP6Packet },
        { PACKET_TYPE_UDP_6,    &BuildSingleUDP6Packet },
        { PACKET_TYPE_ICMP_6,   &BuildSingleICMP6Packet },
        { 0, NULL }
    };

    // process each packet using its particular function for building
    while (ptr != NULL) {
        //printf("window size: %d\n", ptr->tcp_window_size);
        // find the correct function.. no longer a jump table because of ipv6 bitmask checking
        while (PacketBuilders[n].func != NULL) {
            if (ptr->type & PacketBuilders[n].type) {
                // if building this packet fails.. lets mark this attack for deletion
                i = PacketBuilders[n].func(ptr);

                if (i != 1) {
                    ret = 0;
                    goto end;
                }

                break;
            }
            n++;
        }

        ptr->ok = 1;

        ptr = ptr->next;
    }
end:;

    return ret;
}




// This function takes the linked list of build instructions, and loops to build out each packet
// preparing it to be wrote to the Internet.  It will mark an attack structure as completed on
// failure.
void BuildPackets(AS_attacks *aptr) {
    PacketBuildInstructions *ptr = aptr->packet_build_instructions;
    PacketInfo *qptr = NULL;
    int i = 0;
    int n = 0;


    //printf("1 build packets aptr %d completed %d\n", aptr->id, aptr->completed);
    //printf("1 count: %d\n", L_count((LINK *)aptr->packet_build_instructions));

    if ((ptr == NULL) || (BuildPacketInstructions(ptr) != 1)) {
        aptr->completed = 1;
        return;
    }

    // All packets were successful.. lets move them to a different structure..
    // PacketInfo is the structure used to put into the outgoing network buffer..
    // this mightt be possible to remove.. but i wanted to give some room for additional
    // protocols later.. so i decided to keep for now...
    //ptr = aptr->packet_build_instructions;
    //printf("build packets instructions %d\n", L_count((LINK *)ptr));

    while (ptr != NULL) {
        if ((qptr = (PacketInfo *)calloc(1, sizeof(PacketInfo))) == NULL) {
            // Allocation issue.. mark completed
            aptr->completed = 1;
            return;
        }

        qptr->type = ptr->type;
        qptr->buf = ptr->packet;
        qptr->size = ptr->packet_size;
        // These are required for sending the packet out on the raw socket.. so lets pass it
        qptr->dest_ip = ptr->destination_ip;
        CopyIPv6Address(&qptr->dest_ipv6, &ptr->destination_ipv6);
        qptr->dest_port = ptr->destination_port;
        qptr->source_port = ptr->source_port;

        // This should really only matter later.. once they begin 'attempting' to process out
        // false packets.. Doubtfully going to work in any capacity.
        qptr->wait_time = 0;

        // so we dont double free.. lets just keep in the new structure..
        // again i might remove this later... but wanted some room for other upgrades
        // i dont wish to discuss yet ;)
        ptr->packet = NULL;
        ptr->packet_size = 0;

        // link FIFO into the attack structure
        L_link_ordered((LINK **)&aptr->packets, (LINK *)qptr);

        ptr = ptr->next;
    }

    if (aptr->packets && aptr->completed) aptr->completed = 0;

    //printf("2 build packets aptr %d completed %d\n", aptr->id, aptr->completed);
    //printf("2 count: %d\n", L_count((LINK *)aptr->packet_build_instructions));

    return;
}



// free all packets within an attack structure
void PacketsFree(PacketInfo **packets) {
    PacketInfo *ptr = NULL, *pnext = NULL;

    if (packets == NULL) return;

    ptr = *packets;

    // verify there are packets there to begin with
    if (ptr == NULL) return;

    // free all packets
    while (ptr != NULL) {
        // once AS_queue() executes on this.. it moves the pointer over
        // so it wont need to be freed from here (itll happen when outgoing buffer flushes)
        PtrFree(&ptr->buf);

        // keep track of the next, then free the current..
        pnext = ptr->next;

        // free this specific structure element
        free(ptr);

        // now use that pointer to move forward..
        ptr = pnext;
    }

    // no more packets left... so lets ensure it doesn't get double freed
    *packets = NULL;

    return;
}


// This is one of the main logic functions.  It handles sessions which are to be replayed many times, along with the timing 
// logic, and it calls other functions to queue into the network outgoing queue
void PacketLogic(AS_context *ctx, AS_attacks *aptr, OutgoingPacketQueue **_optr) {
    PacketInfo *pkt = NULL;
    struct timeval tv;
    struct timeval time_diff;
    char *sptr = NULL;
    int which_protocol = 0;
    OutgoingPacketQueue *optr = *_optr;

    gettimeofday(&tv, NULL);

    // if this thread is paused waiting for some other thread, or process to complete a task
    if (aptr->paused == 1) return;

    // if its already finished.. lets just move forward
    if (aptr->completed) return;

    // onoe of these two cases are correct fromm the calling function
    if (aptr->current_packet != NULL)
        pkt = aptr->current_packet;
    else {
        // we do have to reprocess these packets fromm packet #1?
        if (aptr->count == 0) {
            // Free all packets (it will put a NULL at its pointer location afterwards)
            PacketsFree(&aptr->packets);

            // If there was anything in current_packet then its freed already from the function above
            aptr->current_packet = NULL;
            
            aptr->completed = 1;

            return;
        }

        // lets start it over..
        pkt = aptr->packets;        
    }

    if (pkt == NULL) {
        // error shouldnt be here...
        aptr->completed = 1;

        return;
    }

    // is it the first packet?
    if (pkt == aptr->packets) {
        // if we are about to replay this attack again from the first packet due to a repeat count.. then
        // verify enough time has elapsed to match our repeat interval (IN seconds)
        timeval_subtract(&time_diff, &aptr->ts, &tv);
        if (pkt->wait_time && time_diff.tv_usec < pkt->wait_time) {
            // we are on the first packet and it has NOT been long enough...
            return;
        }

        // derement the count..
        aptr->count--;

        // aptr->ts is only set if it was already used once..
        if (aptr->ts.tv_sec) {
            // free older packets since we will rebuild
            PacketsFree(&aptr->packets);
            // we wanna start fresh..
            aptr->current_packet = NULL;

            // we have some adjustments to make (source port, identifiers, etc)
            PacketAdjustments(ctx, aptr);

            // we must refresh this pointer after that..
            pkt = aptr->packets;
        }

        // If its marked as completed for any reason, then we are done.
        if (aptr->completed) return;
    } else {
        // Is it too soon to send this packet? (we check its milliseconds)
        timeval_subtract(&time_diff, &aptr->ts, &tv);

        if (pkt->wait_time && time_diff.tv_usec < pkt->wait_time) return;
    }

    if (optr == NULL)
        if ((*_optr = optr = OutgoingPoolGet(ctx)) == NULL)
            return;


    // insert outgoing queue max packet, and size checking logic  here


    // copy packet into outgoing packet queue structure (many together)
    // for speed and to not use many threads, or malloc, etc
    sptr = (char *)(optr->buf);
    sptr += optr->size;
    memcpy(sptr, pkt->buf, pkt->size);

    optr->dest_ip[optr->cur_packet] = pkt->dest_ip;
    CopyIPv6Address(&optr->dest_ipv6[optr->cur_packet], &pkt->dest_ipv6);
    optr->source_port[optr->cur_packet] = pkt->source_port;
    optr->dest_port[optr->cur_packet] = pkt->dest_port;
    optr->attack_info[optr->cur_packet] = aptr;
    optr->ctx = ctx;

    if (pkt->type & PACKET_TYPE_TCP)
        which_protocol = PROTO_TCP;
    else if (pkt->type & IPPROTO_UDP)
        which_protocol = PROTO_UDP;
    else if (pkt->type & IPPROTO_ICMP)
        which_protocol = PROTO_ICMP;

    // mark protocol
    optr->packet_protocol[optr->cur_packet] = which_protocol;
    // mark if its ipv6 by checking if ipv4 is empty
    optr->packet_ipversion[optr->cur_packet] = (pkt->dest_ip == 0);


    optr->packet_starts[optr->cur_packet] = optr->size;
    optr->packet_ends[optr->cur_packet] = (optr->size + pkt->size);

    optr->size += pkt->size;

    // prepare for the next packet
    optr->cur_packet++;

    // We set this pointer to the next packet for next iteration of AS_perform()
    aptr->current_packet = pkt->next;

    // time couldnt have changed that much.. lets just copy it over
    memcpy(&aptr->ts, &tv, sizeof(struct timeval)); 

    return;
}



int BuildSingleUDP4Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    unsigned char *final_packet = NULL;
    struct packetudp4 *p = NULL;
    int final_packet_size = 0;
    struct pseudo_header_udp4 *udp_chk_hdr = NULL;
    char *checkbuf = NULL;

    // this is only for ipv4 tcp (ret 0 since its not technically an error.. just wrong func)
    if (!(iptr->type & PACKET_TYPE_UDP_4)) return 0;

    // calculate full length of packet.. before we allocate memory for storage
    final_packet_size = sizeof(struct iphdr) + sizeof(struct udphdr) + iptr->data_size;

    // allocate space for the packet
    if ((final_packet = (unsigned char *)calloc(1, final_packet_size)) == NULL) goto end;

    // this is the main structure we cast for configuring IP/UDP header parameters
    p = (struct packetudp4 *)final_packet;

    // ensure the final packet was allocated correctly
    if (final_packet == NULL) return ret;

    // IP header below (static)
    p->ip.version       = 4;
    p->ip.ihl   	    = sizeof(struct iphdr) >> 2;
    p->ip.tos   	    = 0;
    p->ip.frag_off 	    = 0;
    p->ip.protocol 	    = IPPROTO_UDP;
    p->ip.ttl           = iptr->ttl;

    p->ip.id = htons(iptr->header_identifier);
    if (p->ip.id == 0) p->ip.id = rand()%0xFFFFFFFF;
    

    // These are the dynamic fields for UDP packets
    p->ip.tot_len       = htons(final_packet_size);
    p->ip.saddr         = iptr->source_ip;
    p->ip.daddr         = iptr->destination_ip;

    // IP header checksum
    p->ip.check         = (unsigned short)in_cksum((unsigned short *)&p->ip, sizeof(struct iphdr));

    p->udp.source       = htons(iptr->source_port);
    p->udp.dest         = htons(iptr->destination_port);
    p->udp.len          = htons(sizeof(struct udphdr) + iptr->data_size);
    p->udp.check        = 0;

    // copy the UDP data after the IP, and UDP header
    memcpy((void *)(final_packet + sizeof(struct udphdr) + sizeof(struct iphdr)), iptr->data, iptr->data_size);

    // allocate memory for performing checksum calculations
    if ((checkbuf = (char *)calloc(1, sizeof(struct pseudo_header_udp4) + sizeof(struct udphdr) + iptr->data_size)) == NULL) goto end;

    // this is the pseudo header used for UDP verification and its pointer for configuring the parameters
    udp_chk_hdr = (struct pseudo_header_udp4 *)checkbuf;

    // copy udp hdr after the pseudo header for checksum
    memcpy((void *)(checkbuf + sizeof(struct pseudo_header_udp4)), &p->udp, sizeof(struct udphdr));

    // if this packet has data then we want to copy it into this buffer for checksum generation
    if (iptr->data_size)
        memcpy((void *)(checkbuf + sizeof(struct pseudo_header_udp4) + sizeof(struct udphdr)), iptr->data, iptr->data_size);

    // UDP requires these psuedo header parameters to be included in its checksum
    udp_chk_hdr->protocol = IPPROTO_UDP;
    udp_chk_hdr->source_address = iptr->source_ip;
    udp_chk_hdr->destination_address = iptr->destination_ip;
    udp_chk_hdr->placeholder = 0;
    udp_chk_hdr->len = htons(sizeof(struct udphdr) + iptr->data_size);

    // call the checksum function to generate a hash for the data we put inside of checkbuf
    p->udp.check = in_cksum((unsigned short *)checkbuf, sizeof(struct pseudo_header_udp4) + sizeof(struct udphdr) + iptr->data_size);

    // ensure the build structure has the buffer, and size we just created so it can push to the wire
    iptr->packet = (char *)final_packet;
    iptr->packet_size = final_packet_size;

    // so we dont free the packet since everything is successful
    final_packet = NULL;
    final_packet_size = 0;

    // everything went well...
    ret = 1;

    end:;

    PtrFree(&final_packet);
    PtrFree(&checkbuf);

    return ret;
}


// Build IPv4 ICMP packet from an instruction structure
int BuildSingleICMP4Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    char *final_packet = NULL;
    int final_packet_size = 0;
    struct packeticmp4 *p = NULL;
    uint32_t pkt_chk = 0;
    struct icmphdr *icmp = NULL;
    int extra_data = 0;

    // this is only for ipv4 tcp
    if (!(iptr->type & PACKET_TYPE_ICMP_4)) return ret;

    if (iptr->data_size < sizeof (struct icmphdr)) extra_data = sizeof(struct icmphdr) - iptr->data_size;

    // calculate size of complete packet
    final_packet_size = sizeof(struct packeticmp4) + iptr->data_size + extra_data;

    

    // allocate space for this packet
    if ((final_packet = (char *)calloc(1, final_packet_size)) == NULL) goto end;

    // prepare the ICMP header pointer in the final packet buffer we are creating
    icmp = (struct icmphdr *)(final_packet + sizeof(struct iphdr));

    // this is the structure we use to prepare the IP header parameters inside of this for the wire packet buffer
    p = (struct packeticmp4 *)final_packet;

    // prepare IP header
    p->ip.version = 4;
    p->ip.ihl = 5;
    p->ip.tos = 0;
    p->ip.tot_len = htons(final_packet_size);
    p->ip.ttl = iptr->ttl;

    p->ip.id = htons(iptr->header_identifier);
    if (p->ip.id == 0) p->ip.id = rand()%0xFFFFFFFF;

    p->ip.frag_off = 0;
    p->ip.protocol = IPPROTO_ICMP;
    p->ip.saddr = iptr->source_ip;
    p->ip.daddr = iptr->destination_ip;

    // perform IP checksum on the packets parameters as they are in memory for the finial buffer.. (as it will be when it hits the wire)
    p->ip.check = in_cksum((unsigned short *)final_packet, sizeof (struct iphdr));
 
    // prepare ICMP header
    // copy over ICMP parameters from its own structure..
    memcpy((void *)&p->icmp, &iptr->icmp, sizeof(struct icmphdr));
    /*
    p->icmp.type = ICMP_ECHO;
    p->icmp.code = 0;
    p->icmp.un.echo.sequence = rand();
    p->icmp.un.echo.id = rand();
    */

    // If there is data to be appended to this packet then lets copy it over
    if (iptr->data_size)
        memcpy((void *)(final_packet + sizeof(struct packeticmp4)), iptr->data, iptr->data_size);

    // this should be zero in the build instructions structure.. but just for future reference
    // it should be 0 before we checksum it..
    p->icmp.checksum = 0;

    // calculate ICMP checksum and put it directly into the final packet
    p->icmp.checksum = (unsigned short)in_cksum((unsigned short *)icmp, sizeof(struct icmphdr) + iptr->data_size + extra_data);
    
    // set the raw packet inside of the structure
    iptr->packet = final_packet;
    iptr->packet_size = final_packet_size;

    // we set these to NULL so it doesnt get freed at the end of this function
    final_packet = NULL;
    final_packet_size = 0;

    // everything went well...
    ret = 1;

    end:;


    PtrFree(&final_packet);
    
    return ret;
}

// Build IPv4 ICMP packet from an instruction structure
int BuildSingleICMP6Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    char *final_packet = NULL;
    int final_packet_size = 0;
    struct packeticmp6 *p = NULL;
    uint32_t pkt_chk = 0;
    struct icmp6_hdr *icmp6 = NULL;

    // this is only for ipv4 tcp
    if (!(iptr->type & PACKET_TYPE_ICMP_6)) return ret;

    // calculate size of complete packet
    final_packet_size = sizeof(struct packeticmp6) + iptr->data_size;

    // allocate space for this packet
    if ((final_packet = (char *)calloc(1, final_packet_size)) == NULL) goto end;

    // prepare the ICMP header pointer in the final packet buffer we are creating
    icmp6 = (struct icmp6_hdr *)(final_packet + sizeof(struct ip6_hdr));

    // this is the structure we use to prepare the IP header parameters inside of this for the wire packet buffer
    p = (struct packeticmp6 *)final_packet;

    // prepare IPv6 header
    p->ip6.ip6_ctlun.ip6_un2_vfc = 6 << 4;
    p->ip6.ip6_ctlun.ip6_un1.ip6_un1_plen = htons(final_packet_size);// - sizeof(struct ip6_hdr));
    p->ip6.ip6_ctlun.ip6_un1.ip6_un1_hlim = iptr->ttl;
    p->ip6.ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_ICMP;

    // get IP addreses out of the packet
    CopyIPv6Address(&p->ip6.ip6_src, &iptr->source_ipv6);
    CopyIPv6Address(&p->ip6.ip6_dst, &iptr->destination_ipv6);

    // prepare ICMP header
    // copy over ICMP parameters from its own structure..
    // I didn't want to support all different ICMP scenarios in PacketBuildInstructions structures
    memcpy((void *)&p->icmp6, &iptr->icmp6, sizeof(struct icmp6_hdr));
    /*
    p->icmp.type = ICMP_ECHO;
    p->icmp.code = 0;
    p->icmp.un.echo.sequence = rand();
    p->icmp.un.echo.id = rand();
    */

    // If there is data to be appended to this packet then lets copy it over
    if (iptr->data_size)
        memcpy((void *)(final_packet + sizeof(struct packeticmp4)), iptr->data, iptr->data_size);

    // this should be zero in the build instructions structure.. but just for future reference
    // it should be 0 before we checksum it..
    p->icmp6.icmp6_cksum= 0;

    // calculate ICMP checksum and put it directly into the final packet
    p->icmp6.icmp6_cksum = (unsigned short)in_cksum((unsigned short *)icmp6, sizeof(struct icmp6_hdr) + iptr->data_size);
    
    // set the raw packet inside of the structure
    iptr->packet = final_packet;
    iptr->packet_size = final_packet_size;

    // we set these to NULL so it doesnt get freed at the end of this function
    final_packet = NULL;
    final_packet_size = 0;

    // everything went well...
    ret = 1;

    end:;

    PtrFree(&final_packet);
    
    return ret;
}



int BuildSingleUDP6Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    unsigned char *final_packet = NULL;
    struct packetudp6 *p = NULL;
    int final_packet_size = 0;
    struct pseudo_header_udp6 *udp_chk_hdr = NULL;
    char *checkbuf = NULL;

    // this is only for ipv4 tcp (ret 0 since its not technically an error.. just wrong func)
    if (!(iptr->type & PACKET_TYPE_UDP_6)) return 0;

    // calculate full length of packet.. before we allocate memory for storage
    final_packet_size = sizeof(struct ip6_hdr) + sizeof(struct udphdr) + iptr->data_size;

    // allocate space for the packet
    if ((final_packet = (unsigned char *)calloc(1, final_packet_size)) == NULL)
        goto end;

    // this is the main structure we cast for configuring IP/UDP header parameters
    p = (struct packetudp6 *)final_packet;

    // IP header below (static)

    // prepare IPv6 header
    p->ip.ip6_ctlun.ip6_un2_vfc = 6 << 4;
    p->ip.ip6_ctlun.ip6_un1.ip6_un1_plen = htons(iptr->data_size + sizeof(struct udphdr));
    p->ip.ip6_ctlun.ip6_un1.ip6_un1_hlim = iptr->ttl;
    p->ip.ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_UDP;
    
    // get IP addreses out of the packet
    CopyIPv6Address(&p->ip.ip6_src, &iptr->source_ipv6);
    CopyIPv6Address(&p->ip.ip6_dst, &iptr->destination_ipv6);


    p->udp.source       = htons(iptr->source_port);
    p->udp.dest         = htons(iptr->destination_port);
    p->udp.len          = htons(sizeof(struct udphdr) + iptr->data_size);
    p->udp.check        = 0;

    // copy the UDP data after the IP, and UDP header
    memcpy((void *)(final_packet + sizeof(struct udphdr) + sizeof(struct ip6_hdr)), iptr->data, iptr->data_size);

    // allocate memory for performing checksum calculations
    if ((checkbuf = (char *)calloc(1, sizeof(struct pseudo_header_udp6) + sizeof(struct udphdr) + iptr->data_size)) == NULL)
        goto end;

    // this is the pseudo header used for UDP verification and its pointer for configuring the parameters
    udp_chk_hdr = (struct pseudo_header_udp6 *)checkbuf;

    // copy udp hdr after the pseudo header for checksum
    memcpy((void *)(checkbuf + sizeof(struct pseudo_header_udp6)), &p->udp, sizeof(struct udphdr));

    // if this packet has data then we want to copy it into this buffer for checksum generation
    if (iptr->data_size)
        memcpy((void *)(checkbuf + sizeof(struct pseudo_header_udp6) + sizeof(struct udphdr)), iptr->data, iptr->data_size);

    // UDP requires these psuedo header parameters to be included in its checksum
    udp_chk_hdr->protocol = IPPROTO_UDP;
    CopyIPv6Address(&udp_chk_hdr->source_address, &iptr->source_ipv6);
    CopyIPv6Address(&udp_chk_hdr->destination_address, &iptr->destination_ipv6);
    udp_chk_hdr->placeholder = 0;
    udp_chk_hdr->len = htons(sizeof(struct udphdr) + iptr->data_size);

    // call the checksum function to generate a hash for the data we put inside of checkbuf
    p->udp.check = in_cksum((unsigned short *)checkbuf, sizeof(struct pseudo_header_udp6) + sizeof(struct udphdr) + iptr->data_size);

    // ensure the build structure has the buffer, and size we just created so it can push to the wire
    iptr->packet = (char *)final_packet;
    iptr->packet_size = final_packet_size;

    // so we dont free the packet since everything is successful
    final_packet = NULL;
    final_packet_size = 0;

    // everything went well...
    ret = 1;

    end:;

    PtrFree(&final_packet);
    PtrFree(&checkbuf);

    return ret;
}



// Takes build instructions from things like HTTP Session generation, and creates the final network ready
// data buffers which will flow across the Internet
int BuildSingleTCP6Packet(PacketBuildInstructions *iptr) {
    int ret = -1;
    int TCPHSIZE = 20;

    if (PacketTCPBuildOptions(iptr) != 1) return -1;

    // this is only for ipv4 tcp
    if (!(iptr->type & PACKET_TYPE_TCP_6)) return ret;

    // increase the heaader by the size of the TCP options
    if (iptr->options_size) TCPHSIZE += iptr->options_size;

    // calculate full length of packet.. before we allocate memory for storage
    int final_packet_size = sizeof(struct ip6_hdr) + TCPHSIZE + iptr->data_size;
    unsigned char *final_packet = (unsigned char *)calloc(1, final_packet_size);
    struct packettcp6 *p = (struct packettcp6 *)final_packet;

    // ensure the final packet was allocated correctly
    if (final_packet == NULL) return ret;

    // prepare IPv6 header
    p->ip.ip6_ctlun.ip6_un2_vfc = 6 << 4;
    p->ip.ip6_ctlun.ip6_un1.ip6_un1_plen = htons(final_packet_size - sizeof(struct ip6_hdr));
    p->ip.ip6_ctlun.ip6_un1.ip6_un1_hlim = iptr->ttl;
    p->ip.ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_TCP;


    // Source, and destination IP addresses
    CopyIPv6Address(&p->ip.ip6_src, &iptr->source_ipv6);
    CopyIPv6Address(&p->ip.ip6_dst, &iptr->destination_ipv6);
    
    // TCP header below
    // The source, and destination ports in question
    p->tcp.source   = htons(iptr->source_port);
    p->tcp.dest     = htons(iptr->destination_port);

    // The ACK/SEQ relate to variables incremented during normal communications..
    p->tcp.seq      = htonl(iptr->seq);
    p->tcp.ack_seq	= htonl(iptr->ack);

    // The TCP window relates to operating system emulation
    p->tcp.window	= htons(iptr->tcp_window_size);
    
    // syn/ack used the most
    p->tcp.syn  	= (iptr->flags & TCP_FLAG_SYN) ? 1 : 0;
    p->tcp.ack	    = (iptr->flags & TCP_FLAG_ACK) ? 1 : 0;
    p->tcp.psh  	= (iptr->flags & TCP_FLAG_PSH) ? 1 : 0;
    p->tcp.fin  	= (iptr->flags & TCP_FLAG_FIN) ? 1 : 0;
    p->tcp.rst	    = (iptr->flags & TCP_FLAG_RST) ? 1 : 0;

    
    p->tcp.check	= 0;	/*! set to 0 for later computing */
    p->tcp.urg	    = 0;    
    p->tcp.urg_ptr	= 0;
    p->tcp.doff 	= TCPHSIZE >> 2;

    /*
    // Lets make sure we cover all header variables.. just to be sure it wont be computationally possible
    // to filter out all packets....
    __u16 	res1:4 // supposed to be unused..
    __u16 	ece:1
    __u16 	cwr:1
    https://stackoverflow.com/questions/1480548/tcp-flags-present-in-the-header
    The two flags, 'CWR' and 'ECE' are for Explicit Congestion Notification as defined in RFC 3168.
    */

    // TCP header checksum
    if (p->tcp.check == 0) {
        struct pseudo_tcp6 *p_tcp = NULL;
        char *checkbuf = (char *)calloc(1,sizeof(struct pseudo_tcp6) + TCPHSIZE + iptr->data_size);

        if (checkbuf == NULL) return -1;

        p_tcp = (struct pseudo_tcp6 *)checkbuf;

        CopyIPv6Address(&p_tcp->saddr, &iptr->source_ipv6);
        CopyIPv6Address(&p_tcp->daddr, &iptr->destination_ipv6);
        p_tcp->mbz      = 0;
        p_tcp->ptcl 	= IPPROTO_TCP;
        p_tcp->tcpl 	= htons(TCPHSIZE + iptr->data_size);

        memcpy(&p_tcp->tcp, &p->tcp, TCPHSIZE);
        memcpy(checkbuf + sizeof(struct pseudo_tcp6), iptr->options, iptr->options_size);
        memcpy(checkbuf + sizeof(struct pseudo_tcp6) + iptr->options_size, iptr->data, iptr->data_size);        

        // put the checksum into the correct location inside of the header
        // options size was already calculated into TCPHSIZE
        p->tcp.check = (unsigned short)in_cksum((unsigned short *)checkbuf, TCPHSIZE + sizeof(struct pseudo_tcp6) + iptr->data_size);
    
        free(checkbuf);
    }

    

    // copy the TCP options to the final packet
    if (iptr->options_size)
        memcpy(final_packet + sizeof(struct packettcp6), iptr->options, iptr->options_size);

    // copy the data to the final packet
    if (iptr->data_size) {
        //md5hash(iptr->data, iptr->data_size);
        memcpy(final_packet + sizeof(struct packettcp6) + iptr->options_size, iptr->data, iptr->data_size);
    }   
    

    // put the final packet into the build instruction structure as completed..
    iptr->packet = (char *)final_packet;
    iptr->packet_size = final_packet_size;


    // returning 1 here will mark it as GOOD
    return (ret = 1);
}



//https://tools.ietf.org/html/rfc1323
// Incomplete but within 1 day it should emulate Linux, Windows, and Mac...
// we need access to the attack structure due to the timestampp generator having a response portion from the opposide sides packets
// *** finish building tcp options data for the header
// !!!
int PacketTCPBuildOptions(PacketBuildInstructions *iptr) {
    // later we nede to access parameters in aptr to build correctly (os emulation)
    AS_attacks *aptr = iptr->aptr;
    // need to see what kind of packet by the flags....
    // then determine which options are necessaray...
    // low packet id (fromm 0 being syn connection) would require the tcp window size, etc

    // options are here static.. i need to begin to generate the timestamp because that can be used by surveillance platforms
    // to attempt to weed out fabricated connections ;) i disabled it to grab this raw array
    unsigned char options[12] = {0x02, 0x04, 0x05, 0xb4, 0x01, 0x01, 0x04, 0x02, 0x01, 0x03,0x03, 0x07};
    // this is preparing for when we have dynamic options...
    char *current_options = NULL;

    int current_options_size = 12;

    // verify that we should even generate the options.. if not return 1 (no error)
    if (!(iptr->flags & TCP_OPTIONS))
        return 1;
    /*
    if (iptr->flags & TCP_OPTIONS_TIMESTAMP) {
        current_options_size += 8;
        // generate new options.. into current_options[_size]
    }*/


    if ((current_options = (char *)malloc(current_options_size)) == NULL) return -1;

    PtrFree(&iptr->options);

    // *** generate options using flags.. timestamp+window size
    // until we generate using flags...
    memcpy(current_options, options, 12);

    iptr->options_size = current_options_size;
    iptr->options = current_options;

    return 1;
}

