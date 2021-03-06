

#define min(a,b) ((a) < (b) ? (a) : (b))



void PacketBuildInstructionsFree(PacketBuildInstructions **list);
AS_attacks *InstructionsToAttack(AS_context *, PacketBuildInstructions *instructions, int count, int interval);
PacketBuildInstructions *InstructionsFindConnection(AS_context *, PacketBuildInstructions **instructions, FilterInformation *flt);
PacketBuildInstructions *PacketsToInstructions(PacketInfo *packets);
int GenerateTCPCloseConnectionInstructions(ConnectionProperties *cptr, PacketBuildInstructions **final_build_list, int from_client);
int GenerateTCPSendDataInstructions(ConnectionProperties *cptr, PacketBuildInstructions **final_build_list, int from_client, char *data, int size);
int GenerateTCPConnectionInstructions(ConnectionProperties *cptr, PacketBuildInstructions **final_build_list);
PacketBuildInstructions *BuildInstructionsNew(PacketBuildInstructions **list, ConnectionProperties *cptr, int from_client, int flags);
int FilterCheck(AS_context *, FilterInformation *fptr, PacketBuildInstructions *iptr);
void FilterPrepare(FilterInformation *fptr, int type, uint32_t value);

PacketBuildInstructions *ThreadedInstructionsFindConnection(AS_context *, PacketBuildInstructions **instructions, FilterInformation *flt, int threads, int replay_count, int interval);
PacketBuildInstructions *ProcessTCP6Packet(PacketInfo *pptr);
PacketBuildInstructions *InstructionsDuplicate(PacketBuildInstructions *sptr);