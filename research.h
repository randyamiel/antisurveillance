

// one single dns record (response about a hostname, prepared to stay on record)
// it can be reused for preparing further attacks against the same sites, etc
// using different residential, or business ip addresses
typedef struct _dns_record {
    struct _dns_record *next;
    // raw response..
    unsigned char *response;
    int response_size;

    unsigned char type; // enums from before

    uint32_t ipv4;
    //struct in6_addr ipv6;

    // ts of last lookup
    int ts;

    // this is necessary for different strageies which will be developed
    int country_id;
} DNSRecord;


typedef struct _lookup_queue {
    struct _lookup_queue *next;

    // MX/ PTR/ A/ AAAA/ etc
    int type;

    char *hostname;

    // spider would be for using different dns servers in different geos
    // it allows using geo ips which look more legit
    // one of the first responses to these attacks will be to filter the attacks out
    // using scenarios like this...
    struct _lookup_queue *spider;
    struct _lookup_queue *recursive;

    // is this queue complete? (it wouuld mean that all recursive/spider are completed as well)
    int complete;

    uint32_t ipv4;
    //struct in6_addr ipv6;

    // how many responses? (different geos, etc)
    int count;
    int ts;

    DNSRecord **responses;
} DNSQueue;


// this is pretty standard...
#define MAX_TTL 30

/*

Traceroute queue which is used to determine the best IPs for manipulation of fiber cables

*/
typedef struct _traceroute_queue {
    struct _traceroute_queue *next;

    // IPv4 or 6 address
    uint32_t target;
    struct in6_addr targetv6;

    //timestamp added
    int ts;

    // last time we noticed activity.. for timeouts, and next ttl
    int ts_activity;

    // ttl (starts at 1 and goes up)
    int current_ttl;
    int max_ttl;

    // identifier to tie this to the responses since we will perform mass amounts
    uint32_t identifier;
} TracerouteQueue;





int Traceroute_Perform(AS_context *ctx);
int Traceroute_Incoming(AS_context *ctx, PacketInfo *pptr);