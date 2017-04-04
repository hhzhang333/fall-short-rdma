#define __USE_GNU
#define _GNU_SOURCE

#include <sched.h>

#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <rdma/rdma_cma.h>
#include <unistd.h>

#include "common.h"
//#include "rudp.h"
#include "server.h"
#include "client.h"
#include "rdtsc.h"

#define OPERATOR READ_OPERATOR 
#define DUPLICATE_COPIES 1

#define CPUGHZ  2.0f


void reply_operation(struct connection *conn, int USE_WRITE);
void *write_cq(void *context);
void run_client(int port, char *host, struct connection *conn);
void run_server(int portno,struct connection *conn, struct ibv_pd *pd, int index);
void on_completion(struct ibv_wc *wc, int k);
void * poll_cq(void *ctx);
int  poll_send_cq(struct context *ctx);
void recv_message(struct connection *conn);
void *run_rone_server(void *arg);
//long long unsigned starttimestamp[MAX_TIMESTAMP_SIZE];
//long long unsigned endtimestamp[MAX_TIMESTAMP_SIZE];

int send_message_size = 2048;
int num_send_request =0;
int window_size= 1;
int signaled=0;
//int cpu_id=0;
int portno=55281;
int internal_port_no = 66889;
int is_client=0;
int event_mode = 0;
int duration=0;
int num_threads=1;
int queue_depth=16;

int is_sync=0;
int rate_limit_mode=1;
long long unsigned syntime;


#define MAX_NUM_IB_THREADS	16
struct context *multi_ctx[MAX_NUM_IB_THREADS];
pthread_t ib_threads[MAX_NUM_IB_THREADS/2];
char *hosts[MAX_NUM_IB_THREADS]={NULL};
pthread_t latency_thread[MAX_NUM_IB_THREADS/2];

uint64_t lats[MAX_NUM_IB_THREADS/2][MAX_TIMESTAMP_SIZE];
pthread_t server_thread[MAX_NUM_IB_THREADS];
/*static sigjmp_buf jmp_alarm[MAX_NUM_IB_THREADS];
static void 
sigalarm_handler (int signo)
{
	printf("alrm is done\n");
	int i;
	for(i=0;i<num_threads;i++)
		siglongjmp(jmp_alarm[i], 1);
}*/


void parseOpt(int argc, char **argv){
	int c; 
	int i=0;	
	while ((c = getopt(argc, argv, "s:m:n:c:b:h:p:e:d:M:S:R:")) != -1) {
		switch (c)
		{
			case 'm':
				send_message_size = atoi(optarg);
				if(send_message_size > RECV_BUFFER_SIZE){
					printf("send_message size is larger than receive buffer size %d\n", RECV_BUFFER_SIZE);
					exit(-1);
				}
				//printf("message_size: %d\n", send_message_size);
				break;
			case 'n':
				num_send_request = atoi(optarg);
				break;
/*			case 'i':
				cpu_id = atoi(optarg);
				printf("cpu id @ parseOpt: %d\n", cpu_id);
				break;*/
			case 'R':
				rate_limit_mode = atoi(optarg);
				break;
			case 'p':
				portno = atoi(optarg);
				break;
			case 'h':
				{
					char *hostnames = strtok(optarg,",");

					while(hostnames){
						if(i>=MAX_NUM_IB_THREADS)
							die("the number of clients is larger than the max");
						hosts[i++]= hostnames;
						hostnames=strtok(NULL, ",");
					}
				}
				break;
			case 'c':
				is_client = atoi(optarg);
				break;
                        case 'd':
                                  queue_depth = atoi(optarg);
                                  break;
			case 'S':
				is_sync = atoi(optarg);
				break;
			case 'b':
				internal_port_no = atoi(optarg);
				break;
			case 's':
				signaled = atoi(optarg);
				break;
			case 'e':
				event_mode = atoi(optarg);
				//printf("enable event mode\n");
				break;
			case 'D':
				duration = atoi(optarg);
				break;
			case 'M':
				num_threads = atoi(optarg);
				break;
			default:
				fprintf(stderr, "usage: %s -m<message_size> -n<num_send_request> -s<signaled>  -e<event_mode> -h<host_ip> -c<is_client> -p<port_no> -M<num_threads> -b<internal_port_no> -R<rate_limit_mode>\n", argv[0]);
				exit(-1);
		}
	}
	if(event_mode && duration){
		printf("event mode can be only with number of requests\n");
		exit(-1);
	}

	if(num_send_request && duration){
		printf("please only specify one parameter, either num_requests or duration\n");
		exit(-1);
	}
	if(is_client == 1 && num_threads > i){
		printf("thread number is larger than server number!!\n");
		exit(-1);
	}
}

static void * latency_measure(void * arg){
	int wait_for_acks_ret;
	int index = (int) arg;

	bindingCPU(index);
	struct context *s_ctx=multi_ctx[index];
	struct connection *conn = s_ctx->conn;
	conn->send_message_size = 0;
	int i=0;
	while(1){
		//uint64_t start_ts = query_hardware_time(s_ctx->ctx);
		uint64_t start_ts =on_write_read(s_ctx,conn,1, WRITE_OPERATOR, COMMON_ROCE);
		
		void *ev_ctx;
		struct ibv_cq *cq;
		int polling_size = 1;
		struct ibv_exp_wc wc[polling_size];
		int k=0;
		int ack_i;
		do{
			if(event_mode == 1){
				TEST_NZ(ibv_get_cq_event(s_ctx->send_comp_channel, &cq, &ev_ctx));
				ibv_ack_cq_events(cq, polling_size);
				TEST_NZ(ibv_req_notify_cq(cq, 0));
				k = ibv_exp_poll_cq(cq, polling_size, wc, sizeof(wc[0]));
			}else
				k = ibv_exp_poll_cq(s_ctx->send_cq, polling_size, wc, sizeof(wc[0]));	
		}while(k<=0);

		for(ack_i=0; ack_i< k; ack_i++){
			if (wc[ack_i].status != IBV_WC_SUCCESS){
				fprintf(stderr,"unsuccessful connection\n");	
				break;	
			}
			uint64_t cur_nic_ts;
			if(wc[ack_i].exp_wc_flags & IBV_EXP_WC_WITH_TIMESTAMP){
				cur_nic_ts = wc[ack_i].timestamp;
			}

			uint64_t basertt= (cur_nic_ts - start_ts)/NIC_CLOCK_MHZ;
			//printf("baseRTT %"PRIu64"\n", basertt);
			lats[index%num_threads][i]=basertt;
			i++;
		}
		usleep(500);
		if(i>=MAX_TIMESTAMP_SIZE)
			break;		

	}
}

static void * incast(void * arg){
	int wait_for_acks_ret;
	int index = (int) arg;

	bindingCPU(index);
	struct context *s_ctx=multi_ctx[index];
	struct connection *conn = s_ctx->conn;
	//s_ctx->srv_state->index = index;
	//s_ctx->srv_state->sl = index%num_threads;
	struct timespec start, end;
	int dur= 0;
	clock_gettime(CLOCK_MONOTONIC, &start);

	long long unsigned st = start.tv_sec*BILLION + start.tv_nsec;
	if(is_sync == 1){
		while(st < syntime){
			clock_gettime(CLOCK_MONOTONIC, &start);
			st = start.tv_sec*BILLION + start.tv_nsec;
		}
	}
	if(duration > 0){	
	//	clock_gettime(CLOCK_MONOTONIC, &end);
	//	dur =end.tv_sec - start.tv_sec;
		
		/*if(sigsetjmp(jmp_alarm[index],1)!=0){
			printf("jmp buf @index: %d\n", index);
			goto end_sending;
		}
	//	if(index == 0)
	//		alarm(duration);
		struct itimerval timer;
		timer.it_interval.tv_sec = 0;
		timer.it_interval.tv_usec = 0;
		timer.it_value.tv_sec = duration;// (ivl / USEC_IN_SEC);
		timer.it_value.tv_usec = 0;//(ivl % USEC_IN_SEC);
		setitimer(ITIMER_REAL, &timer, 0);*/

	}
	//sleep(30);
	//printf("start sending %d\n", index);


	int k;

	//printf("sending @index %d\n", index);
	while(conn->num_sendcount < conn->num_requests ||dur < duration || conn->num_completions < conn->num_requests){
		while((conn->num_sendcount < conn->num_requests ||dur < duration)
				&& conn->num_sendcount- conn->num_completions < queue_depth ){
			wait_for_acks_ret = on_write_read(s_ctx,conn,1, OPERATOR, COMMON_ROCE);
			conn->num_sendcount++;
		}
		k = poll_send_cq(s_ctx);
		if(k==0)
			goto end_sending;
		else
			conn->num_completions +=k;
	}
  
	
end_sending:
	clock_gettime(CLOCK_MONOTONIC, &end);

	long long unsigned diff = (long long unsigned)(BILLION * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec);
	long unsigned data = (long unsigned)conn->num_sendcount*conn->send_message_size;

	//printf("data %llu, %llu\n", conn->num_sendcount, conn->send_message_size);
	double tput = data*8.0/diff;
	double tot_time = (double)diff/BILLION;
	//printf("throughput @ thread %d time %llu ns, size %llu bits, tput %f Gb/s\n", index, diff, data, tput);
	printf("- [%.9g, %lu, %.9g, 1]\n", tot_time, data, tput);


}

int poll_send_cq(struct context *ctx)
{
	void *ev_ctx;
	struct ibv_cq *cq;
	int polling_size = queue_depth;
	struct ibv_exp_wc wc[polling_size];
	int k=0;
	int ack_i;
	do{
		if(event_mode == 1 || is_client == 0){
			TEST_NZ(ibv_get_cq_event(ctx->send_comp_channel, &cq, &ev_ctx));
			ibv_ack_cq_events(cq, polling_size);
			TEST_NZ(ibv_req_notify_cq(cq, 0));
			k = ibv_exp_poll_cq(cq, polling_size, wc, sizeof(wc[0]));
		}else
			k = ibv_exp_poll_cq(ctx->send_cq, queue_depth, wc, sizeof(wc[0]));
	}while(k<=0);

	for(ack_i=0; ack_i< k; ack_i++){
		if (wc[ack_i].status != IBV_WC_SUCCESS){
			fprintf(stderr," wrong wc->status: %s, opcode %s\n",
					ibv_wc_status_str(wc[ack_i].status), ibv_wc_opcode_str(wc[ack_i].exp_opcode));
			fprintf(stderr,"unsuccessful connection\n");
			k=0;
			break;
		}

	}
	return k;
}

int main(int argc, char **argv)
{
	parseOpt(argc,argv); 
	if(is_client==1 && hosts == NULL){
		printf("the number of parameters doest not match with server or client \n \
				client:<port><server><isClient> \n \
				server:<port>\n");
		exit(1);
	}

	struct ibv_device **dev_list = ibv_get_device_list(NULL) ;
	struct ibv_device *ib_dev = dev_list[0];

	memset(lats, 0 ,sizeof(lats));
	int index;
	struct context *s_ctx;
	for(index=0;index<(num_threads);index++){
		s_ctx = NULL;
		s_ctx = init_ctx(ib_dev,s_ctx);
		multi_ctx[index] = s_ctx;		
		//initilize connection
		struct connection *conn= s_ctx->conn;
		conn->send_message_size = send_message_size;
		conn->num_requests = num_send_request;
		conn->send_transport = RC_TRANSPORT;
		conn->signal_all = signaled;
		conn->window_size = window_size;
		conn->isClient= is_client;
		//if (conn->isClient == 0)	
		//	TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq,NULL));
		//TEST_NZ(pthread_create(&s_ctx->send_cq_poller_thread, NULL, poll_send_cq, NULL));

		conn->actual_completions =0;
		//queue pair initilization
		struct ibv_qp_init_attr qp_attr;
		build_qp_attr(&qp_attr,s_ctx);  
		TEST_Z(conn->qp = ibv_create_qp(s_ctx->pd, &qp_attr));
		get_qp_info(s_ctx);

		modify_qp_to_init(conn);
		//setting the memory area
		set_memory(conn, s_ctx, OPERATOR,2*index+REQ_AREA_SHM_KEY);
		/*	int blocks = RECV_BUFFER_SIZE/send_message_size;
			int i=0;
			for(i=0;i<blocks;i++){
			char c=i%26+'a';
			if (conn->isClient == 1){
		//set server memory
		fillzero(s_ctx->conn->recv_region+i*send_message_size, c, send_message_size);
		}else{
		//clear client memory
		fillzero(s_ctx->conn->recv_region+i*send_message_size, '0', send_message_size);
		}
		}*/

		int index_i = index% num_threads;
		if(conn->isClient ){   
			//printf("server %s, portno %d\n", hosts[index_i], portno+index); 
			if(index == index_i){
				run_client(portno,hosts[index],conn);
				if(connect_ctx(conn, conn->local_qp_attr->psn, conn->remote_qp_attr, index_i, COMMON_ROCE)){
					printf("connect_ctx error at client side\n");
					return 1;
				}
			}else{
				run_client(portno+1, hosts[index_i], conn);
				if(connect_ctx(conn, conn->local_qp_attr->psn, conn->remote_qp_attr, 7, COMMON_ROCE)){
					printf("connect_ctx error at client side\n");
					return 1;
				}
	
			}
			//if(OPERATOR == READ_OPERATOR  && conn->signal_all == 0)
			//	TEST_NZ(pthread_create(&s_ctx->write_poller_thread, NULL, write_cq, s_ctx->conn));
			//TEST_NZ(pthread_create(&s_ctx->throughput_timer_thread, NULL, throughput_timer, s_ctx->conn));
			//sleep(1);	
			//init_rtt();
			//exit(0);	
			//reply_operation(s_ctx->conn, OPERATOR);   
		
		}else{
			//printf("portno %d\n", portno+index);
			if(index == index_i){
				if(pthread_create(&server_thread[index/2], NULL, run_rone_server, (void*)index)!= 0)
					die("main(): Failed to create server thread .");	

			}else{
				run_server(portno+index, conn, s_ctx->pd, 7);				
			}

		}

	}

//	if((is_client ==1 && OPERATOR == WRITE_OPERATOR) || (is_client == 0 && OPERATOR == READ_OPERATOR))
//		if(pthread_create(&dcbnetlink_thread, NULL, run_dcbnetlink, NULL) != 0)
//			die("main(): Failed to create worker thread .");	

//	signal(SIGALRM, sigalarm_handler);
	if(is_client==1){
		struct timespec incast_start, incast_end;
		clock_gettime(CLOCK_MONOTONIC, &incast_start);
		if(is_sync == 1)
			syntime = incast_start.tv_sec*BILLION + incast_start.tv_nsec + BILLION; 

		int i;
		for(i = 1; i < num_threads; i++){
			if(pthread_create(&latency_thread[i%num_threads], NULL, latency_measure, (void *) i) != 0)
				die("main(): Failed to create worker thread .");	

		}
		(void) latency_measure((void*)(intptr_t)0);
	
		
		/* Waiting for completion */
		for( i= 1; i < num_threads; i++)
			if(pthread_join(ib_threads[i], NULL) !=0 )
				die("main(): Join failed for worker thread i");

		printf("latency:\n");	
		int j=0;
		for(j=0;j<num_threads; j++){
			for(i=0; i<MAX_TIMESTAMP_SIZE; i++){
				if(lats[j][i] == 0)
					break;
				printf("- [%d, %"PRIu64"]\n", j, lats[j][i]);
			}	
		}
		/*for(index=0; index<num_threads;index++){
			on_disconnect(multi_ctx[index]);
		}*/
			
	}else{
		poll_send_cq(multi_ctx[0]);	
		poll_send_cq(multi_ctx[1]);	
	}
	return 0;
}

void *run_rone_server(void *arg){
	int index = (int) arg;

	bindingCPU(index);
	struct context *s_ctx=multi_ctx[index];
	struct connection *conn = s_ctx->conn;
	run_server(portno+index, conn, s_ctx->pd, index%8);

}




/*void reply_operation(struct connection *conn, int USE_WRITE){
	if(conn->num_sendcount == 0 && conn->isClient == 1 ){
		int i;
		for(i=0;i<conn->window_size/DUPLICATE_COPIES;i++){
			conn->num_sendcount ++;
			if(USE_WRITE){
				int j=0;
				for (j=0; j< DUPLICATE_COPIES;j++)
					on_write_read(s_ctx, conn,0, USE_WRITE);
				//rudp_send_read_write(s_ctx,conn, 0, USE_WRITE);
			}else
				on_send(conn,0);
			printf("num_sendcount %d\n", conn->num_sendcount);
			record_timestamp(conn->num_sendcount-conn->num_requests/2, starttimestamp);
		}
		return;
	}

	conn->num_sendcount++;
	if(conn->num_sendcount*DUPLICATE_COPIES%MAX_SEND_WR == 0){
		if(USE_WRITE){
			int	j=0;
			for (j=0;j<DUPLICATE_COPIES;j++)
				on_write_read(s_ctx, conn,1, USE_WRITE);
			//rudp_send_read_write(s_ctx,conn, 0, USE_WRITE);

		}else
			on_send(conn,1);
	}else{
		//while(1){ if(conn->isSignal == 0) break;}
		if(USE_WRITE){
			int j=0;
			for(j=0;j<DUPLICATE_COPIES;j++)
				on_write_read(s_ctx, conn,0, USE_WRITE);
			//rudp_send_read_write(s_ctx,conn, 0, USE_WRITE);
		}else
			on_send(conn,0);
	}
	record_timestamp(conn->num_sendcount-conn->num_requests/2, starttimestamp);
}*/

/*void recv_message(struct connection *conn){
	conn->actual_completions ++;
	char * region = conn->recv_region;
	int blocks = RECV_BUFFER_SIZE/conn->send_message_size;
	char c = (char)region[(conn->num_completions)%blocks*conn->send_message_size]; 
	char exp =( (conn->num_completions)/blocks)%26+'a';
	if( c == exp ){
		record_timestamp(conn->num_completions - conn->num_requests/2, endtimestamp);
		conn->num_completions ++;
	}

	if (conn->num_completions < conn->num_requests && conn->num_sendcount*DUPLICATE_COPIES-conn->actual_completions < conn->window_size){
		// keep the requests on the network less than MAX_SEND_WR
		reply_operation(conn, OPERATOR);
	}else if (conn->num_completions >= conn->num_requests){
		printf("conn->num_completions: %d\n", conn->num_completions);
		per_request_latency(conn, starttimestamp, endtimestamp);
		exit(0); 		
	}
}*/

void on_completion(struct ibv_wc *wc, int k)
{
	struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;
	//printf("opcode = %s\n", ibv_wc_opcode_str(wc->opcode));
	if (wc->status != IBV_WC_SUCCESS){
		printf("wc->status: %s\n", ibv_wc_status_str(wc->status));
		die("");
	}

/*	if(wc->opcode == IBV_WC_RDMA_READ){    
		conn->isSignal = 0;
		if(conn->signal_all){
			recv_message(conn);
			//printf("conn->num_completions: 1\n");
			 // conn->num_completions++;

			  //record_timestamp(conn->num_completions, endtimestamp);
		}//sending 
	}*/

}

void * write_cq(void *context)
{
	struct connection *conn = (struct connection *)context;
	char *region = conn->recv_region;
	int blocks = RECV_BUFFER_SIZE/conn->send_message_size;
	while (1) {
		char c = (char)region[conn->num_completions%blocks*conn->send_message_size]; 
		if(c > ((conn->num_completions/blocks)%blocks+96)){
			printf("messages %.*s, completions: %d\n", 4, region+conn->num_completions%blocks*conn->send_message_size, conn->num_completions );
			//		fillzero(region+conn->num_sendcount%blocks*conn->send_message_size,'0', conn->send_message_size);
			//recv_message(conn);
		}
	}

	return NULL;
}

//void * poll_cq(void *ctx)
//{
	/*	struct ibv_cq *cq;
		struct ibv_wc wc;

		while (1) {
		TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));
		ibv_ack_cq_events(cq, 1);
		TEST_NZ(ibv_req_notify_cq(cq, 0));
		int k =ibv_poll_cq(cq, 1, &wc);
		while (k){
	//printf("poll cq k = %d\n", k);
	on_completion(&wc,k);
	k = ibv_poll_cq(cq, 1, &wc);
	}
	}
	 */
/*	int prev_psn=-1;	
	while(1){
		sleep(1);
		int rq_psn= get_current_my_psn(s_ctx->conn->qp,0);		*/
		/*if (rq_psn == prev_psn)
		  reconnect_qp(s_ctx->conn->qp, s_ctx->conn->local_qp_attr->psn, s_ctx->conn->remote_qp_attr, s_ctx->conn->send_transport);	
		  else
		  prev_psn = rq_psn;	
		 */
/*	}

	return NULL;
}*/

/*void * poll_send_cq(void *ctx)
{
	struct ibv_cq *cq;
	struct ibv_wc wc;

	while (1){
		TEST_NZ(ibv_get_cq_event(s_ctx->send_comp_channel, &cq, &ctx));
		ibv_ack_cq_events(cq, 1);
		TEST_NZ(ibv_req_notify_cq(cq, 0));
		int k = ibv_poll_cq(cq, 1, &wc);
		while (k){
			printf("k = %d\n", k);
			//		on_completion(&wc,k);
			k = ibv_poll_cq(cq, 1, &wc);
		}
	}
	return NULL;
}*/
void run_server(int portno,struct connection *conn, struct ibv_pd *pd, int index)
{
	int sockfd, newsockfd; // portno;
	socklen_t clilen;

	//char buffer[S_QPA];
	printf("portno %d\n", portno);
	struct sockaddr_in serv_addr, cli_addr;
	int n;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) 
		die("ERROR opening socket");
	bzero((char *) &serv_addr, sizeof(serv_addr));
	//portno = atoi(argv[1]);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);
	if (bind(sockfd, (struct sockaddr *) &serv_addr,
				sizeof(serv_addr)) < 0) 
		die("ERROR on binding");
	listen(sockfd,5);

	clilen = sizeof(cli_addr);
	newsockfd = accept(sockfd, 
			(struct sockaddr *) &cli_addr, 
			&clilen);
	if (newsockfd < 0) 
		die("ERROR on accept");

	//memset(s_ctx->remote_qp_attr, 0, S_QPA);
	conn->remote_qp_attr = (struct qp_attr *)malloc(sizeof(struct qp_attr));
	n = recv(newsockfd,conn->remote_qp_attr,S_QPA,0);
	if (n < 0) die("ERROR reading from socket");

	//print_qp_attr(conn->remote_qp_attr);

	n = send(newsockfd,conn->local_qp_attr,S_QPA,0);
	if (n < 0) die("ERROR writing to socket");
	//print_qp_attr(conn->local_qp_attr);

	//if(USE_WRITE){  
	conn->peer_mr = (struct ibv_mr*)malloc(sizeof(struct ibv_mr));
	n=recv(newsockfd, conn->peer_mr,sizeof(struct ibv_mr),0);
	if (n < 0) die("ERROR reading from peer_mr");
	n=send(newsockfd,conn->recv_mr, sizeof(struct ibv_mr),0);
	if (n<0) die("ERROR writing send_r");   
	// }

	if(conn->send_transport == 2){
		create_ah(conn, pd);
		modify_dgram_qp_to_rts(conn,  conn->local_qp_attr->psn);
	}else{
		if(connect_ctx(conn, conn->local_qp_attr->psn, 
					conn->remote_qp_attr,index, COMMON_RONE)) {
			fprintf(stderr, "Couldn't connect to remote QP\n");
			exit(0);
		}
	}
	char buffer[128];
	memset(buffer, 0, sizeof(buffer));
	sprintf(buffer,"%s","OK");
	n = send(newsockfd,buffer,sizeof(buffer),0);
	if (n < 0) die("ERROR sending OK to socket");


	close(newsockfd);
	close(sockfd);
	return ; 
}

void run_client(int portno, char *hostname, struct connection *conn){
	int sockfd,  n;
	struct sockaddr_in serv_addr;
	struct hostent *server;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) 
		die("ERROR opening socket");
	server = gethostbyname(hostname);
	if (server == NULL) {
		fprintf(stderr,"ERROR, no such host\n");
		exit(0);
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, 
			(char *)&serv_addr.sin_addr.s_addr,
			server->h_length);
	serv_addr.sin_port = htons(portno);
	if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) 
		die("ERROR connecting");

	n = send(sockfd,conn->local_qp_attr, S_QPA,0);
	if (n < 0) 
		die("ERROR writing to socket");
//		print_qp_attr(conn->local_qp_attr);

	conn->remote_qp_attr = (struct qp_attr *)malloc(sizeof(struct qp_attr));
	memset(conn->remote_qp_attr, 0, S_QPA);
	n = recv(sockfd,conn->remote_qp_attr,S_QPA,0);
	if (n < 0) 
		die("ERROR reading from socket");
//	print_qp_attr(conn->remote_qp_attr);

	// if(USE_WRITE){
	n=send(sockfd,conn->recv_mr, sizeof(struct ibv_mr),0);
	if (n<0)die("ERROR writing send_r");

	conn->peer_mr = (struct ibv_mr*)malloc(sizeof(struct ibv_mr));
	n=recv(sockfd, conn->peer_mr,sizeof(struct ibv_mr),0);
	if (n < 0) die("ERROR reading from peer_mr");
	//}
	char v[128];
	memset(v, 0, sizeof(v));
	n = recv(sockfd,v, sizeof(v),0);
	if (n < 0) 
		die("ERROR reading OK message from socket");

	close(sockfd);
	return ;
}


