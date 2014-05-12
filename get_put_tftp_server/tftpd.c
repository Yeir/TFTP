#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <errno.h>
#include "tftp.h"
#include <fcntl.h>
#define RET_OK   0
#define RET_ERR -1


#define BUFFER_SIZE 1024
#define ECHO_PORT 4535

int sockfd = -1;
int sockfd1 = -1;
int max_fd = 0; //记录最大的sockfd 
int peer_fd[1024]; //记录sockfd 
int getput[1024]; //记录上传或下载 
fd_set readfd;  //select集合 
int num = 1;  //端口号加数 
int opt = 1;
struct sockaddr_in remoteaddr;

/*文件信息 */
struct fi {
	FILE *file_fd; //文件fd 
	int block; //记录客户端下载文件第几个包 
	int file_len; //文件长 
	struct sockaddr_in cli; //客户端信息 
	int putn;  
}f_state[1024];

/*错误信息打印 */
int SendTftpError() {
	struct TFTPERR err;
	bzero(&err, sizeof(err));
	err.header.opcode = htons(OPCODE_ERR);
	err.errcode = htons(1); 
	sendto(sockfd, &err, sizeof(err), 0,
		(struct sockaddr*)&remoteaddr, sizeof(remoteaddr));
}

/*下载文件获取文件名，记录信息 */ 
void get_filestate(int n,void *buffer) {
	struct stat f_stat;
	struct TFTPWRRQ request;
	request.filename = (char*)buffer + sizeof(struct TFTPHeader);
	if(stat(request.filename, &f_stat) < 0) {
		perror("stat()");
		SendTftpError();
		return ;
	}
	FILE *fp = fopen(request.filename, "rb");
	if(fp == NULL){	
		perror("open()");
		SendTftpError();
		return ;
	} 
	int blockno = f_stat.st_size / BLOCKSIZE + 1;
	f_state[n].file_fd = fp;
	f_state[n].block = 1;
	f_state[n].file_len = blockno;
}

/*记录上传文件名，记录信息 */
void put_filename(int n, void *buffer) {
	struct TFTPWRRQ request;
	request.filename = (char*)buffer + sizeof(struct TFTPHeader);
	
	FILE *fp = fopen(request.filename, "wb");
	if(fp == NULL) {
		perror("fopen()");
		return ;
	}
	f_state[n].file_fd=fp;
}

/*发ACK*/ 
int sendDataAck(int n,struct sockaddr_in *pPeeraddr, struct TFTPData *pData) {
	struct TFTPACK p;
	p.header.opcode = htons(OPCODE_ACK);
	p.block = pData->block;
    
	if(sendto(peer_fd[n], &p, sizeof(p), 0,
		(struct sockaddr*)pPeeraddr, sizeof(struct sockaddr_in)) < 0){
		perror("ack send()");
		return 1;
	}
	
	return 0;
}

/*发送信息*/ 
int SendTftpData(char *pData, int len, short blockId,int n) {
	struct TFTPData buffer;
	buffer.header.opcode = htons(OPCODE_DATA);
	buffer.block = htons(blockId);
	buffer.data = malloc(len);
	memcpy(buffer.data, pData, len);
	
	int ret = sendto(peer_fd[n], &buffer, (len+2+2), 0,
		(struct sockaddr*)&f_state[n].cli, sizeof(f_state[n].cli));
	printf("DBG: sento() return %d\n", ret);
}

/*下载函数 */
int tftpread(int n){
	
	//记录是否第一次接收，发送接收后的ACK 
	if(f_state[n].block!=1) {
		struct TFTPACK ack;
		if(recvfrom(peer_fd[n], &ack, sizeof(ack), 0, NULL, 0) < 0){	
			perror("recvfrom()");
			return 1;
		}
		
		if(ntohs(ack.block) != (f_state[n].block-1)) {
			printf("ack err.\n");
			return 1;
		}else {
			printf("ack checked.\n");
		}
	}
	
	//判断是否为文件的尾 
	if(f_state[n].block==(f_state[n].file_len+1)) {
		close(peer_fd[n]);
		close((int)f_state[n].file_fd);
		peer_fd[n]=0;
	}else {
		void *filebuffer = malloc(BLOCKSIZE);
		int r = fread(filebuffer, 1, BLOCKSIZE, f_state[n].file_fd);
		SendTftpData(filebuffer, r, f_state[n].block,n);
		f_state[n].block++;
	}
}

/*上传函数 */
int tftpwrite(int n)
{
	struct sockaddr_in peeraddr;
	memset(&peeraddr, 0, sizeof(peeraddr));
	int bufferlen = 1000;
	while(bufferlen == 0)
		ioctl(peer_fd[n], FIONREAD, &bufferlen);
	printf("DBG: bufferlen = %d\n", bufferlen);
	
	void *buffer = malloc(bufferlen);
	int recvLen = sizeof(peeraddr);
	int recvlen = recvfrom(peer_fd[n], buffer, bufferlen, 0, 
		(struct sockaddr *)&peeraddr, &recvLen);
	if(recvlen < 0) {
		perror("recvfrom()");
		return 0;
	}
	struct TFTPData *d;
	d = (struct TFTPData *)buffer;
	if(d->header.opcode == htons(3))
		fwrite(&(d->data), recvlen, 1, f_state[n].file_fd);
	int y = sendDataAck(n, &peeraddr, d);
	if(recvlen < 516) {
		close(peer_fd[n]);
		peer_fd[n] = 0;
		return 0;
	}
}

/*判断上传时客户端是否发送RRQ消息 */
int Judge(void *buffer, struct sockaddr_in re, int ret) {
	int i;
	int r;
	for(i=0; i<10; i++) {
		if(re.sin_port == f_state[i].cli.sin_port
			&&re.sin_addr.s_addr == f_state[i].cli.sin_addr.s_addr) {
			break;
		}
	}
	//如果已经记录RRQ消息，就进入接收文件数据
	if(i != 10) {
		struct TFTPData *d;
		d = (struct TFTPData *)buffer;
		if(d->header.opcode == htons(3)) {
			fwrite(&(d->data), ret, 1, f_state[i].file_fd);
		}
		int y = sendDataAck(i, &remoteaddr, d);
		if(ret < 516) {
			close(peer_fd[i]);
			peer_fd[i] = 0;
		}
		f_state[i].putn = 1;  //客户端已经记录服务器的新端口号
		return 1;
	} else {
		return 0;
	}
}

/*select函数接收 */
void tftp_select(int sockfd) {
	int i,j;
	uint8_t buffer[BUFFER_SIZE];
	int ret = RET_OK;
	memset(buffer, 0 , sizeof(buffer));
	FD_ZERO(&readfd);
	FD_SET(sockfd, &readfd);
	int len = sizeof(remoteaddr);
	
	for(i=0; i<10; i++) {
		if(peer_fd[i] == 0)	continue;
		FD_SET(peer_fd[i], &readfd);
	}
	
	if(select(max_fd+1, &readfd, NULL, NULL, NULL)>0) {
		int ans = FD_ISSET(sockfd, &readfd);
		
		if(ans > 0) {		
			if((ret = recvfrom(sockfd, buffer, sizeof(buffer), 0, 
				(struct sockaddr *)&remoteaddr, &len)) > 0) {
				
				if( !(Judge(buffer, remoteaddr, ret)) ) {
					struct TFTPHeader *header = (struct TFTPHeader*)buffer;
					struct sockaddr_in ser;
					int one = (htons(OPCODE_RRQ));
					int two = (htons(OPCODE_WRQ));
					int three = (htons(OPCODE_INPUT));
					int p;
					//查找没有记录sockfd
					for(i=0; i<10; i++) {
						if(peer_fd[i] == 0) {
							p = i;
							break;
						}
					}

					//创建新的sockfd接收客户端的消息
					memset(&ser, 0, sizeof(ser));
					ser.sin_family = AF_INET;
					ser.sin_addr.s_addr = INADDR_ANY;
					ser.sin_port = htons(ECHO_PORT + num);
					num++;
					if((sockfd1 = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
						perror("DBG:ERROR opening socket");
						return ;
					}
					if((ret = setsockopt(sockfd1, SOL_SOCKET, SO_REUSEADDR, 
						&opt, sizeof(opt))) < 0) {
						perror("DBG:ERROR setsockopt");
						return ;
					}
					if ((ret = bind(sockfd1, (struct sockaddr *) &ser, sizeof(ser))) < 0) {
						perror("DBG:ERROR on binding");
						return ;
					}
					if(fcntl(sockfd1, F_SETFL, O_NONBLOCK)<0) {
						perror("fcntl");
					}
					peer_fd[p] = sockfd1;
					f_state[p].cli = remoteaddr;
					FD_SET(sockfd1, &readfd);
					if(max_fd < sockfd1) {
						max_fd = sockfd1;
					}
					
					if(header->opcode == one) {
						get_filestate(p, buffer);
						getput[p] = 1;	
					}
					else if(header->opcode == three) {
						put_filename(p,buffer);
						f_state[p].putn=0;
						getput[p]=2;
					}	 else {
						fprintf(stderr, "DBG:Incorrect opcode: %d\n", ntohs(header->opcode));
					}
				}
			}
		} else { 
			perror("ERROR rev");
		}

		//查找sockfd是否有消息
		for(i=0; i<10; i++) {
			if(FD_ISSET(peer_fd[i], &readfd)) {
				if(getput[i] == 1) {
					tftpread(i);
				}
				else if(getput[i] == 2 &&  f_state[i].putn) {
					tftpwrite(i);				
				}
				else if(getput[i] == -1)  continue;
			}
		}
	}
}

int main(int argc, char **argv) {
	
	int len;
	struct sockaddr_in cliaddr;
	int ret = RET_OK;
	int i;
	
	if((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("DBG:ERROR opening socket");
		return RET_ERR;
	}
	if((ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
		perror("DBG:ERROR setsockopt");
		return 0;
	}
	memset(&cliaddr, 0, sizeof(cliaddr));
	cliaddr.sin_family = AF_INET;
	cliaddr.sin_addr.s_addr = INADDR_ANY;
	cliaddr.sin_port = htons(ECHO_PORT);
	if ((ret = bind(sockfd, (struct sockaddr *) &cliaddr, sizeof(cliaddr))) < 0) {
		perror("DBG:ERROR on binding");
		return 0;
	}
	
	memset(peer_fd, 0, sizeof(peer_fd));
	memset(getput, -1, sizeof(getput));
	printf("DBG:listen port %d\r\n", ECHO_PORT);
	max_fd = sockfd;	
	num = 1;
	while(1) {
		usleep(10);
		tftp_select(sockfd);
	}
	close(sockfd);
	return 0;
}

