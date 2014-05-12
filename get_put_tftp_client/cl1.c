#include<stdio.h>
#include<string.h>
#include "tftp.h"
#define RET_OK   0
#define RET_ERR -1
#define POR 4535

int sockfd;
int sockfd1;
int ret;
int opt = 1;
struct sockaddr_in remoteaddr;
struct sockaddr_in cliaddr;

int sendReadReq(char *pFilename, struct sockaddr_in *pRemoteAddr, short Opcode) {
	struct TFTPHeader header;
	header.opcode = htons(Opcode);
	int filenamelen = strlen(pFilename) + 1;
	int packetsize = sizeof(header) + filenamelen + 5 + 1;
	void *packet = malloc(packetsize);	// mode = "octet"
	memcpy(packet, &header, sizeof(header));
	memcpy(packet + sizeof(header), pFilename, filenamelen);
	char *mode = "octet";
	memcpy(packet + sizeof(header) + filenamelen, mode, strlen(mode) + 1);
 
	if(sendto(sockfd, packet, packetsize, 0,
		(struct sockaddr*)pRemoteAddr, sizeof(struct sockaddr)) < 0) {
		perror("sendto()");
		return 1;
	}
	return 0;
}

int SendTftpError() {
	struct TFTPERR err;
	bzero(&err, sizeof(err));
	err.header.opcode = htons(OPCODE_ERR);
	err.errcode = htons(1);
	sendto(sockfd, &err, sizeof(err), 0,
		(struct sockaddr*)&remoteaddr, sizeof(remoteaddr));
}

int SendTftpData(char *pData, int len, short blockId) {
	struct TFTPData buffer;
	buffer.header.opcode = htons(OPCODE_DATA);
	buffer.block = htons(blockId);
	buffer.data = malloc(len);
	memcpy(buffer.data, pData, len);
	
	int ret = sendto(sockfd, &buffer, (len+2+2), 0,
		(struct sockaddr*)&cliaddr, sizeof(struct sockaddr_in));
	printf("DBG: sento() return %d\n", ret);
}

int sendDataAck(struct sockaddr_in *pPeeraddr, struct TFTPData *pData) {
	struct TFTPACK p;
	p.header.opcode = htons(OPCODE_ACK);
	p.block = pData->block;
    
	if(sendto(sockfd, &p, sizeof(p), 0,
		(struct sockaddr*)pPeeraddr, sizeof(struct sockaddr_in)) < 0){
		perror("ack send()");
		return 1;
	}

	return 0;
}

int tftpread(char *filename) {
	struct sockaddr_in peeraddr;
	memset(&peeraddr, 0 ,sizeof(peeraddr));
	FILE *fp = fopen(filename, "wb");
	if(fp == NULL){
		perror("fopen()");
		return 1;
	}
	while(1) {
		int bufferlen = 0;
		while(bufferlen == 0)	// wait for data
		{
			ioctl(sockfd, FIONREAD, &bufferlen);
		}
		printf("DBG: bufferlen = %d\n", bufferlen);

		void *buffer = malloc(bufferlen);

		int recvLen = sizeof(peeraddr);
		int recvlen = recvfrom(sockfd, buffer, bufferlen, 0, 
							(struct sockaddr *)&peeraddr, &recvLen);
		if(recvlen < 0) {
			perror("recvfrom()");
			break;
		}
		struct TFTPData *d;
		d = (struct TFTPData *)buffer;
		if(d->header.opcode == htons(3)) {
			fwrite(&(d->data), recvlen, 1, fp);
		}
		int y = sendDataAck(&peeraddr, d);
		if(recvlen < 516) {
			close(sockfd);
			break;
		}
	}
	close(fp);
	close(sockfd);
}

int tftpwrite(char *filename) {
	struct stat f_stat;

	if(stat(filename, &f_stat) < 0) {
		perror("stat()");
		SendTftpError();
		return 1;
	}
 
	printf("DBG: filesize = %ld\n", f_stat.st_size);
 
	FILE *fp = fopen(filename, "rb");
	if(fp == NULL) {	
		perror("open()");
		SendTftpError();
		return 1;
	}
 
	printf("DBG: request file opened.\n");
 
	int blockno = f_stat.st_size / BLOCKSIZE + 1;
	int i = 0;
	for(i = 1; i <= blockno; i++) {
		printf("DBG: sending block %d\n", i);
 
		void *filebuffer = malloc(BLOCKSIZE);
		int r = fread(filebuffer, 1, BLOCKSIZE, fp);
		SendTftpData(filebuffer, r, i);
		struct TFTPACK ack;
		int l = sizeof(cliaddr);
		if(recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&cliaddr, &l) < 0) {
			perror("recvfrom()");
			return 1;
		}
 
		if(ntohs(ack.block) != i) {
			printf("ack err.\n");
			return 1;
		} else {
			printf("ack checked.\n");
		}
	}
	return 0;
}

int main(int argc, char **argv) {
    if(argc < 5) {
        printf("please input five data!\n");
    }
	
    struct hostent *host;
	struct sockaddr_in serv_addr;
	if((host = gethostbyname(argv[1])) == NULL) {
		perror("gethostbyname");
		exit(1);
	}
	
	memset(&cliaddr, 0, sizeof(cliaddr));
	cliaddr.sin_family = AF_INET;
	cliaddr.sin_addr = *((struct in_addr *)host->h_addr);
	cliaddr.sin_port = htons(POR);
	
	if((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("ERROR opening socket");
		return RET_ERR;
	}
	if((ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
		perror("ERROR setsockopt");
	}
	
	int x;
	printf("%s\n",argv[3]);
	if(strcmp(argv[3], "get") == 0) {
    		if((x=sendReadReq(argv[4],&cliaddr,OPCODE_RRQ))<0) {
    	 		perror("ERROR sendReadReq");
    		}
		x = tftpread(argv[4]);
	} else {
    		if((x=sendReadReq(argv[4],&cliaddr,OPCODE_INPUT))<0) {
    		 	perror("ERROR sendReadReq");
    		}
		x = tftpwrite(argv[4]);
	}
    return 0;
}
