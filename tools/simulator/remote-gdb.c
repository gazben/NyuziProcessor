#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include "Core.h"

#define TRAP_SIGNAL 5 // SIGTRAP

static Core *gCore;
static int clientSocket = -1;
static int lastSignal[THREADS_PER_CORE];	// XXX threads hard coded

int readByte()
{
	unsigned char ch;
	if (read(clientSocket, &ch, 1) < 1)
	{
		perror("error reading from socket");
		return -1;
	}
	
	return ch;
}

int readPacket(char *packetBuf, int maxLength)
{
	int ch;
	int packetLen;

	// Wait for packet start
	do
	{
		ch = readByte();
		if (ch < 0)
			return -1;
	}
	while (ch != '$');

	// Read body
	packetLen = 0;
	while (1)
	{
		ch = readByte();
		if (ch < 0)
			return -1;
		
		if (ch == '#')
			break;
		
		if (packetLen < maxLength)
			packetBuf[packetLen++] = ch;
	}
	
	// Read checksum and discard
	readByte();
	readByte();
	
	packetBuf[packetLen] = '\0';
	return packetLen;
}

const char *kGenericRegs[] = {
	"fp",
	"sp",
	"ra",
	"pc"
};

void sendResponsePacket(const char *packetBuf)
{
	unsigned char checksum;
	char checksumChars[16];
	
	write(clientSocket, "$", 1);
	write(clientSocket, packetBuf, strlen(packetBuf));
	write(clientSocket, "#", 1);

	checksum = 0;
	for (int i = 0; packetBuf[i]; i++)
		checksum += packetBuf[i];
	
	sprintf(checksumChars, "%02x", checksum);
	write(clientSocket, checksumChars, 2);
	
	printf(">> %s\n", packetBuf);
}

void sendFormattedResponse(const char *format, ...)
{
	char buf[256];
    va_list args;
    va_start(args, format);
	vsnprintf(buf, sizeof(buf) - 1, format, args);
	va_end(args);
	sendResponsePacket(buf);
}

void runUntilInterrupt(Core *core)
{
	fd_set readFds;
	int result;

	FD_ZERO(&readFds);

	while (1)
	{
		runQuantum(core, 1000);
		FD_SET(clientSocket, &readFds);
		result = select(clientSocket + 1, &readFds, NULL, NULL, NULL);
		if ((result < 0 && errno != EINTR) || result == 1)
			break;
	}
}

void remoteGdbMainLoop(Core *core)
{
	int listenSocket;
	struct sockaddr_in address;
	socklen_t addressLength;
	int got;
	char packetBuf[256];
	int i;
	int noAckMode = 0;
	int optval;
	char response[256];
	
	gCore = core;
	
	for (i = 0; i < THREADS_PER_CORE; i++)
		lastSignal[i] = 0;

	listenSocket = socket(PF_INET, SOCK_STREAM, 0);
	if (listenSocket < 0)
	{
		perror("socket");
		return;
	}

	optval = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);
	
	address.sin_family = AF_INET;
	address.sin_port = htons(8000);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listenSocket, (struct sockaddr*) &address, sizeof(address)) < 0)
	{	
		perror("bind");
		return;
	}

	if (listen(listenSocket, 4) < 0)
	{
		perror("bind");
		return;
	}
	
	while (1)
	{
		// Wait for a new client socket
		while (1)
		{
			addressLength = sizeof(address);
			clientSocket = accept(listenSocket, (struct sockaddr*) &address,
				&addressLength);
			if (clientSocket >= 0)
				break;
		}
		
		printf("Got connection from debugger\n");
		noAckMode = 0;

		// Process commands
		while (1)
		{
			got = readPacket(packetBuf, sizeof(packetBuf));
			if (got < 0) 
				break;
			
			if (!noAckMode)
				write(clientSocket, "+", 1);

			printf("<< %s\n", packetBuf);

			switch (packetBuf[0])
			{
				// Set Value
				case 'Q':
					if (strcmp(packetBuf + 1, "StartNoAckMode") == 0)
					{
						noAckMode = 1;
						sendResponsePacket("OK");
					}
					else
						sendResponsePacket("");	// Not supported
					
					break;
					
				// Query
				case 'q':
					if (strcmp(packetBuf + 1, "LaunchSuccess") == 0)
						sendResponsePacket("OK");
					else if (strcmp(packetBuf + 1, "HostInfo") == 0)
						sendResponsePacket("triple:nyuzi;endian:little;ptrsize:4");
					else if (strcmp(packetBuf + 1, "ProcessInfo") == 0)
						sendResponsePacket("pid:1");
					else if (strcmp(packetBuf + 1, "fThreadInfo") == 0)
						sendResponsePacket("m1,2,3,4");
					else if (strcmp(packetBuf + 1, "sThreadInfo") == 0)
						sendResponsePacket("l");
					else if (memcmp(packetBuf + 1, "ThreadStopInfo", 14) == 0)
						sprintf(response, "S%02x", lastSignal[getCurrentStrand(core)]);
					else if (memcmp(packetBuf + 1, "RegisterInfo", 12) == 0)
					{
						int regId = strtoul(packetBuf + 13, NULL, 16);
						if (regId < 32)
						{
							sprintf(response, "name:s%d;bitsize:32;encoding:uint;format:hex;set:General Purpose Scalar Registers;gcc:%d;dwarf:%d;",
								regId, regId, regId);
								
							if (regId >= 28)
								sprintf(response + strlen(response), "generic:%s;", kGenericRegs[regId - 28]);
						}
						else if (regId < 64)
						{
							sprintf(response, "name:v%d;bitsize:512;encoding:uint;format:vector-uint32;set:General Purpose Vector Registers;gcc:%d;dwarf:%d;",
								regId - 32, regId, regId);
						}
						else
							sprintf(response, "");
						
						sendResponsePacket(response);
					}
					else if (strcmp(packetBuf + 1, "C") == 0)
						sendFormattedResponse("QC%02x", getCurrentStrand(core) + 1);
					else
						sendResponsePacket("");	// Not supported
					
					break;
					
				// Set arguments
				case 'A':
					sendResponsePacket("OK");	// Yeah, whatever
					break;
					
					
				// continue
				case 'C':
				case 'c':
					runUntilInterrupt(core);
					lastSignal[getCurrentStrand(core)] = TRAP_SIGNAL;
					sprintf(response, "S%02x", lastSignal[getCurrentStrand(core)]);
					break;
					
				// Step
				case 's':
				case 'S':
					singleStep(core);
					lastSignal[getCurrentStrand(core)] = TRAP_SIGNAL;
					sprintf(response, "S%02x", lastSignal[getCurrentStrand(core)]);
					break;
					
				// Pick thread
				case 'H':
					if (packetBuf[1] == 'g')
					{
						sendResponsePacket("OK");
						printf("set thread %d\n", packetBuf[2] - '1');
					}
					else
						sendResponsePacket("");

					break;
					
					
				// read register
				case 'p':
				case 'g':
				{
					int regId = strtoul(packetBuf + 1, NULL, 16);
					int value;
					if (regId < 32)
					{
						value = getScalarRegister(core, regId);
						sendFormattedResponse("%08x", value);
					}
					else if (regId < 64)
					{
						int lane;
						
						for (lane = 0; lane < 16; lane++)
						{
							value = getVectorRegister(core, regId, lane);
							sprintf(response + lane * 8, "%08x", value);
						}

						sendResponsePacket(response);
					}
					else
						sendResponsePacket("");
				
					break;
				}
					
				// Multi-character command
				case 'v':
					if (strcmp(packetBuf, "vCont?") == 0)
						sendResponsePacket("vCont;C;c;S;s");
					else if (memcmp(packetBuf, "vCont;", 6) == 0)
					{
						if (packetBuf[6] == 's')
						{
							int threadId = strtoul(packetBuf + 8, NULL, 16);
							setCurrentStrand(core, threadId);
							singleStep(core);
							lastSignal[getCurrentStrand(core)] = TRAP_SIGNAL;
							sendFormattedResponse("S%02x", lastSignal[getCurrentStrand(core)]);
						}
						else if (packetBuf[6] == 'c')
						{
							runUntilInterrupt(core);
							lastSignal[getCurrentStrand(core)] = TRAP_SIGNAL;
							
							// XXX stop response
							sendFormattedResponse("S%02x", lastSignal[getCurrentStrand(core)]);
						}
						else
							sendResponsePacket("");
					}
					else
						sendResponsePacket("");
					
					break;
					
				// Get last signal
				case '?':
					sprintf(response, "S%02x", lastSignal[getCurrentStrand(core)]);
					sendResponsePacket(response);
					break;
					
				// Unknown
				default:
					sendResponsePacket("");
			}
		}
		
		printf("Disconnected from debugger\n");
		close(clientSocket);
	}
}
