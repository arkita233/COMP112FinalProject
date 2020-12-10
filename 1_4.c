#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <fcntl.h>
#include <regex.h>

#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>

#define ISVALIDSOCKET(s) ((s) >= 0)
#define CLOSESOCKET(s) close(s)
#define SOCKET int
#define GETSOCKETERRNO() (errno)
#define PROTOCOL "HTTP/1.0"
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
#define TIMEOUT 300
#define SERVER_NAME "high_performance_proxy"
#define SERVER_URL "wait and see"
#define BOLD_WORDS "bilibili"

#define MAX_REQUEST_SIZE 2047
#define MAX_SIZE 1024 * 1024 * 10
#define BSIZE 1024

struct client_info {
    socklen_t address_length;
    struct sockaddr_storage address;
    char address_buffer[128];
    char path[128];
    char request[MAX_REQUEST_SIZE + 1];
    SOCKET socket;
    int received;
    struct client_info *next;
    SOCKET server_socket;
    bool is_server;
    bool is_https;
    SSL *ssl;
    SSL_CTX *ctx;
    int message_len;
    int cur_size;
    char *message;
    bool out_of_memory;
    long start_time;
};

void send_400(struct client_info **client_list,struct client_info *client);
void send_404(struct client_info **client_list,struct client_info *client);
void send_408(struct client_info **client_list,struct client_info *client);
void send_500(struct client_info **client_list,struct client_info *client);
void send_503(struct client_info **client_list,struct client_info *client);

/////////////////////pthread////////////////////////////
pthread_mutex_t mutex;

//==============================================
typedef struct thrArgs
{
    struct client_info **client_list;
    struct client_info *client;
    const char *path;
    int ret;
}Param;

typedef struct gfcArgs
{
    struct client_info **client_list;
    struct client_info *client;
    struct client_info *server;
    int ret;
}gfcParam;

typedef struct dwcArgs {
    fd_set reads;

} dwcParam;
/////////////////////pthread////////////////////////////


SOCKET create_socket(const char* host, const char *port) {
    printf("Configuring local address...\n");
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *bind_address;
    getaddrinfo(host, port, &hints, &bind_address);

    printf("Creating socket...\n");
    SOCKET socket_listen;
    socket_listen = socket(bind_address->ai_family,
            bind_address->ai_socktype, bind_address->ai_protocol);
    
    int optval = 1;
    setsockopt(socket_listen, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));

    if (!ISVALIDSOCKET(socket_listen)) {
        fprintf(stderr, "socket() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    printf("Binding socket to local address...\n");
    if (bind(socket_listen,
                bind_address->ai_addr, bind_address->ai_addrlen)) {
        fprintf(stderr, "bind() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }
    freeaddrinfo(bind_address);

    printf("Listening...\n");
    if (listen(socket_listen, 10) < 0) {
        fprintf(stderr, "listen() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    return socket_listen;
}

/*store content for different url and port*/
typedef struct Input
{
	char url[256];
    int port;
	char object[MAX_SIZE];
	int maxAge;
    long now;
    int length;
} Input;

/*treat input as node in linkedlist*/
typedef struct QNode
{
    struct QNode *prev, *next;
    Input* input;  
} QNode;

/*FIFO*/
typedef struct Queue
{
    unsigned count;  
    unsigned numberOfFrames; 
    QNode *front, *rear;
} Queue;

/*make handle element in queue easier*/
typedef struct Hash
{
    int capacity; 
    QNode* *array;
} Hash;

Input* createInput(char* url, int port, char* object, int maxAge, long now, int length)
{
  Input* input = (Input *)malloc(sizeof( Input));
  strcpy(input->url,url);
  input->port = port;
  memcpy(input->object, object, length);
  input->maxAge = maxAge;
  input->now = now;
  input->length = length;
  return input;
}

QNode* newQNode( Input* input )
{
    QNode* temp = (QNode *)malloc( sizeof( QNode ) );
    temp->input = input;
    temp->prev = temp->next = NULL;
    return temp;
}
 
Queue* createQueue( int numberOfFrames )
{
    Queue* queue = (Queue *)malloc( sizeof( Queue ) ); 
    queue->count = 0;
    queue->front = queue->rear = NULL;
    queue->numberOfFrames = numberOfFrames;
 
    return queue;
}
 

Hash* createHash( int capacity )
{
    Hash* hash = (Hash *) malloc( sizeof( Hash ) );
    hash->capacity = capacity;
    hash->array = (QNode **) malloc( hash->capacity * sizeof( QNode* ) );
    int i;
    for( i = 0; i < hash->capacity; ++i )
        hash->array[i] = NULL;
 
    return hash;
}
/*to see if queue is full*/
int AreAllFramesFull( Queue* queue )
{
    return queue->count == queue->numberOfFrames;
}

int isQueueEmpty( Queue* queue )
{
    return queue->rear == NULL;
}
 
void deQueue( Queue* queue )
{
    if( isQueueEmpty( queue ) )
        return;
 
    if (queue->front == queue->rear)
        queue->front = NULL;
 
    QNode* temp = queue->rear;
    queue->rear = queue->rear->prev;
 
    if (queue->rear)
        queue->rear->next = NULL;
 
    free( temp );

    queue->count--;
}
 
void Enqueue( Queue* queue, Hash* hash, Input* input )
{

    int i = 0;
    bool ifFull = false;
    if ( AreAllFramesFull ( queue ) )
    {
        for (i = 0; i < hash->capacity; i++)
        {
            if(strcmp(hash->array[i]->input->url,queue->rear->input->url) == 0 && hash->array[i]->input->port == queue->rear->input->port)
            {
                hash->array[i] = NULL;
                break;
            }
        }
        ifFull = true;
        deQueue( queue );
    }
 
    QNode* temp = newQNode( input );
    temp->next = queue->front;
 
    if ( isQueueEmpty( queue ) )
        queue->rear = queue->front = temp;
    else 
    {
        queue->front->prev = temp;
        queue->front = temp;
    }

    if(ifFull)
    {
        hash->array[i] = temp;
    }
    else
    {
        //put into blank in hash
        for (i = 0; i < hash->capacity; i++)
        {
            if(hash->array[i] == NULL)
            {
                hash->array[i] = temp;
                break;
            }
        } 
    }
    queue->count++;
}

void checkIfExpired(Queue* queue, Hash* hash)
{
    for(int i = 0; i < hash->capacity;i++)
    {
        time_t now;
        if(hash->array[i] != NULL && time(&now) - hash->array[i]->input->now > hash->array[i]->input->maxAge)
        {
            QNode* temp = hash->array[i];
            hash->array[i] = NULL;
            if (temp == queue->rear)
            {
                deQueue(queue);
            }
            else if (temp == queue->front)
            {
                queue->front = queue->front->next;
                if(queue->front)
                    queue->front->prev = NULL;
                free(temp);
            } 
            else
            {
                temp->prev->next = temp->next;
                temp->next->prev = temp->prev;
                free(temp);
            }
            queue->count--;

        }
    }
}

/*put new element in front of the queue*/
void put_into_cache( Queue* queue, Hash* hash, Input* input )
{
    int i;
    bool existed = false;
    QNode* reqPage;
    //find if exist

    checkIfExpired(queue,hash);

    for (i = 0; i < hash->capacity; i++)
    {
        if(hash->array[i] == NULL)
        {
            continue;
        }
        if(strcmp(hash->array[i]->input->url,input->url) == 0 && hash->array[i]->input->port == input->port)
        {
            existed = true;
            reqPage = hash->array[i];
        }
    }
 
    if (!existed)
    {
        Enqueue( queue, hash, input);
        return;
    }       
    else if (reqPage != queue->front)
    {
        reqPage->prev->next = reqPage->next;
        if (reqPage->next)
           reqPage->next->prev = reqPage->prev;
         if (reqPage == queue->rear)
        {
           queue->rear = reqPage->prev;
           queue->rear->next = NULL;
        }
        reqPage->next = queue->front;
        reqPage->prev = NULL;
        reqPage->next->prev = reqPage;
 
        queue->front = reqPage;
    }
        //update the data
    memcpy(reqPage->input->object,input->object, input->length);
    reqPage->input->port = input->port;
    reqPage->input->maxAge = input->maxAge;
    reqPage->input->now = input->now;
    reqPage->input->length = input->length;
}

/*get element out of cache*/
Input* get_from_cache(Queue* queue, Hash* hash, char* url, int port) 
{
    Input* input;
    for (int i = 0; i < hash->capacity; i++)
    {
        if(hash->array[i] == NULL)
        {
            continue;
        }
        if(strcmp(hash->array[i]->input->url,url) == 0 && hash->array[i]->input->port == port)
        {
            QNode* reqPage = hash->array[i];
            input = reqPage->input;
            if (reqPage != queue->front)
            {
                reqPage->prev->next = reqPage->next;
                if (reqPage->next)
                reqPage->next->prev = reqPage->prev;
                if (reqPage == queue->rear)
                {
                queue->rear = reqPage->prev;
                queue->rear->next = NULL;
                }
                reqPage->next = queue->front;
                reqPage->prev = NULL;
                reqPage->next->prev = reqPage;
        
                queue->front = reqPage;
            }
            break;           
        }         
    }
    return input;
}

/*check if element exists in cache*/
bool checkIfExisted(Hash* hash, Queue* queue, char* url, int port){
  bool existed = false;
  checkIfExpired(queue, hash);
  if(queue->count == 0) return existed;
  for (int i = 0; i < hash->capacity; i++)
    {
        if(hash->array[i] == NULL)
        {
            continue;
        }
        if(strcmp(hash->array[i]->input->url ,url) == 0 && hash->array[i]->input->port == port)
        {
            existed = true;
        }
    }
    return existed;
}

/*print cache content*/
void print_cache(Queue *q){
    time_t now;
    QNode *temp = q->front;
    int i = 0;
    while(temp){
        printf("item: %d | url:%s | port:%d | current_age: %lu | max_age: %d | size: %d\n", i, temp->input->url, temp->input->port, time(&now) - temp->input->now, temp->input->maxAge, temp->input->length);
        temp = temp->next;
        i++;
    }
}
/*get cached client requests*/
struct client_info *get_client(struct client_info **client_list,
        SOCKET s) {
    struct client_info *ci = *client_list;

    while(ci) {
        if (ci->socket == s)
            break;
        ci = ci->next;
    }

    if (ci) return ci;
    struct client_info *n =
        (struct client_info*) calloc(1, sizeof(struct client_info));

    if (!n) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }

    n->address_length = sizeof(n->address);
    n->next = *client_list;
    n->message_len = 0;
    n->message = NULL;
    n->ssl = NULL;
    n->ctx = NULL;
    n->cur_size = 0;
    n->out_of_memory = false;
    time_t now;
    n->start_time = time(&now);
    *client_list = n;
    return n;
}

/*remove client from cache*/
void drop_client(struct client_info **client_list,
        struct client_info *client) {
    if(!client) return;
    printf("dropping fd %d \n", client->socket);


    struct client_info **p = client_list;

    while(*p) {
        if (*p == client) {
            if(client->next) printf("current is %d, next is %d\n", client->socket, client->next->socket);
            *p = client->next;
            printf("drop finished-1\n");
            printf("\n%s\n", client->message);
            printf("drop finished-2\n");
            if(client->message) {
                //printf("\n%s\n", client->message);
                /*
                FILE *receive;
                if((receive = fopen("receive.txt","wb")) == NULL)
                {
                    perror("fail to read");
                    exit (1) ;
                }
                fwrite(client->message,client->message_len,1,receive);
                fclose(receive);
                */
                free(client->message);
            }
            //printf("%p\n%p\n", client->ssl, client->ctx);
            printf("drop finished0\n");
            if (client->ctx) {
                SSL_CTX_free(client->ctx);
            }
            // printf("drop finished1\n");
            
            // if (client->ssl){
            //     SSL_shutdown(client->ssl);
            // }
            printf("drop finished2\n");
            if(client->socket != 0) {
                CLOSESOCKET(client->socket);
                printf("socket closed\n");
            }
            printf("drop finished3\n");
            //printf("%p\n%p\n", client->ssl, client->ctx);
            if (client->ssl){
                SSL_free(client->ssl);
            }
            printf("drop finished4\n");
            free(client);
            printf("fd %d dropped\n", client->socket);  
            printf("drop finished5\n");
            return;
        }
        p = &(*p)->next;
    }

    fprintf(stderr, "drop_client not found.\n");
    //exit(1);
}


const char *get_client_address(struct client_info *ci) {
    getnameinfo((struct sockaddr*)&ci->address,
            ci->address_length,
            ci->address_buffer, sizeof(ci->address_buffer), 0, 0,
            NI_NUMERICHOST);
    return ci->address_buffer;
}

/*wait for request*/
fd_set wait_on_clients(struct client_info **client_list, SOCKET server) {
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(server, &reads);
    SOCKET max_socket = server;

    struct client_info *ci = *client_list;

    while(ci) {
        printf("%d\t", ci->socket);
        //delete timeout client
        struct client_info *temp = NULL;
        time_t now;
        if(time(&now) - ci->start_time > TIMEOUT) {
            temp = ci;
            printf("timeout to remove\n");
            if(temp) {
                drop_client(client_list, temp);
                drop_client(client_list, get_client(client_list, temp->server_socket));
                if (FD_ISSET(temp->server_socket, &reads)) FD_CLR(temp->server_socket, &reads);
            }
        }
        else {
            FD_SET(ci->socket, &reads);
            if (ci->socket > max_socket)
                max_socket = ci->socket;
        }
        ci = ci->next;     
    }
    //printf("\n");

    if (select(max_socket+1, &reads, 0, 0, 0) < 0) {
        fprintf(stderr, "select() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    return reads;
}
void showFields(X509 *cert){
    char *subj = X509_NAME_oneline(X509_get_subject_name(cert), NULL, 0);
    char *issuer = X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0);
    fprintf(stderr, "CERTIFICATE:\n%s\n%s\n", subj, issuer);
}
/*get new SSL certificate*/
void get_new_certificate(SSL *server_ssl, SSL_CTX *client_ctx){
    printf("make new certificate......\n");
    
    X509 *server_X509 = SSL_get_peer_certificate(server_ssl);
    
    FILE *privkey = fopen("privkey.pem", "r");
    RSA *root_rsa = PEM_read_RSAPrivateKey(privkey, NULL, 0, NULL);
    fclose(privkey);
    EVP_PKEY *root_key = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(root_key, root_rsa);//root CA private key

    RSA *server_rsa;
    BIGNUM *e;
    e = BN_new();
    BN_set_word(e, RSA_F4);
    server_rsa = RSA_new();
    RSA_generate_key_ex(server_rsa, 2048, e, NULL);
    EVP_PKEY *server_key = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(server_key, server_rsa);//server prrivate key
    
    X509 *client_cert = X509_new();
    uint64_t pr;
    ASN1_INTEGER_get_uint64(&pr, X509_get_serialNumber(server_X509));
    ASN1_INTEGER_set(X509_get_serialNumber(client_cert), pr);
    X509_gmtime_adj(X509_get_notBefore(client_cert), 0);
	X509_gmtime_adj(X509_get_notAfter(client_cert), 31536000L);

	X509_set_pubkey(client_cert, server_key);
    X509_set_subject_name(client_cert,  X509_NAME_dup(X509_get_subject_name(server_X509)));
    
    X509_NAME * issuer = X509_get_issuer_name(client_cert);
    X509_NAME_add_entry_by_txt(issuer, "C",  MBSTRING_ASC, (unsigned char *)"CN", -1, -1, 0);
    X509_NAME_add_entry_by_txt(issuer, "ST",  MBSTRING_ASC, (unsigned char *)"Beijing", -1, -1, 0);
    X509_NAME_add_entry_by_txt(issuer, "L",  MBSTRING_ASC, (unsigned char *)"Beijing", -1, -1, 0);
    X509_NAME_add_entry_by_txt(issuer, "O",  MBSTRING_ASC, (unsigned char *)"High Performance Proxy", -1, -1, 0);
    X509_NAME_add_entry_by_txt(issuer, "OU",  MBSTRING_ASC, (unsigned char *)"XXX", -1, -1, 0);
    X509_NAME_add_entry_by_txt(issuer, "CN", MBSTRING_ASC, (unsigned char *)"Comp112", -1, -1, 0);

    //https://stackoverflow.com/questions/15875494/how-to-retrieve-issuer-alternative-name-for-ssl-certificate-by-openssl
    STACK_OF(GENERAL_NAME) *san_names = NULL;
    int crit;
    san_names = X509_get_ext_d2i(server_X509, NID_subject_alt_name, &crit, NULL);
    X509_add1_ext_i2d(client_cert, NID_subject_alt_name, san_names, crit, X509V3_ADD_REPLACE);

    showFields(client_cert);

    X509_sign(client_cert, root_key, EVP_sha256());

    SSL_CTX_use_certificate(client_ctx, client_cert);
    SSL_CTX_use_PrivateKey(client_ctx, server_key);


}

/*open the socket communicating with server*/
int open_client_socket(struct client_info **client_list,
    struct client_info *client, char* hostname, unsigned short port )
    {
    struct hostent *he;
    struct sockaddr_in sa_in;

    int sa_len, sock_family, sock_type, sock_protocol;
    int sockfd, ret, flag, optval;

    socklen_t optlen;

    (void) memset( (void*) &sa_in, 0, sizeof(sa_in) );
    printf("%s\n", hostname);
    he = gethostbyname( hostname );
    if ( he == (struct hostent*) 0 ) {
        send_404(client_list, client);
        return -1;
    }
    sock_family = sa_in.sin_family = he->h_addrtype;
    sock_type = SOCK_STREAM;
    sock_protocol = 0;
    sa_len = sizeof(sa_in);
    (void) memmove( &sa_in.sin_addr, he->h_addr, he->h_length );
    sa_in.sin_port = htons( port );

    sockfd = socket( sock_family, sock_type, sock_protocol );
    if ( sockfd < 0 ) {
        send_500(client_list, client);
        return -1;
    }
    //set sockfd non-blocking
    int flags;
    flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    if ( connect( sockfd, (struct sockaddr*) &sa_in, sa_len ) < 0 ) {
        if (errno == EINPROGRESS){
            fd_set set;
            FD_ZERO(&set);
            FD_SET(sockfd, &set);
            struct timeval timeout;
            timeout.tv_sec = 2; 
            timeout.tv_usec = 0; 
            ret = select(sockfd+1, 0, &set, 0, &timeout);
            if (ret == 0 || (ret < 0 && errno != EINTR)) {
                send_503(client_list,client);
                return -1;
            } else if (ret > 0) {               
                optlen = sizeof(int);
                if(getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (void *)(&optval), &optlen) < 0) {
                    send_503(client_list,client);
                    return -1;
                }
                if(optval != 0) {
                    send_503(client_list,client);
                    return -1;
                }
            }
            ret = connect( sockfd, (struct sockaddr*) &sa_in, sa_len );
        } else{
            send_503(client_list,client);
            return -1;
        }
        
    } else{printf("connect successfully\n");}
    fcntl(sockfd, F_SETFL, flags);

    return sockfd;
}


/*get response from server*/
char* fetch_from_server(struct client_info **client_list,
        struct client_info *client, char *host, char* address, unsigned short port, int *length){
    
    printf("fetch from server %s:%d %s\n", host, port, address);
    
    char *serverContent = (char *)malloc(MAX_SIZE * sizeof(char));
    char buf[BSIZE];
    SOCKET sockfd;

    sockfd = open_client_socket( client_list, client, host, port);
    if (sockfd >= 0) {
        memset(buf,0,1024);
        printf("%s\n", address);
        sprintf(buf, "GET http://%s%s HTTP/1.1\r\nAccept: */*\r\nHost: %s\r\nConnection: Close\r\nUser-Agent: HighPerformanceHttpProxy\r\n\r\n",host,address,host);
        int ret;
        ret = send(sockfd, buf, strlen(buf), 0);
        if (ret < 0) 
            perror("ERROR writing to socket");

        int totalBytes = 0;
        memset(serverContent,0,MAX_SIZE);
        char unsignedBuf[1024];

        while( 1 )
        {
            memset(unsignedBuf,0,BSIZE);
            ret = recv(sockfd, unsignedBuf, BSIZE, MSG_WAITALL);
            
            if(ret < 1)
                break;
            memcpy(serverContent + totalBytes ,unsignedBuf,ret);
            totalBytes += ret;
        }
        printf("server received %d bytes \n", totalBytes);
        *length = totalBytes;
        close(sockfd);
    } else {
        *length = 0;
        serverContent = NULL;
    }

    return serverContent;

}

/*serve http request*/
void serve_http_resource(struct client_info **client_list,
        struct client_info *client, char *path, Hash *hash, Queue *q) {

    printf("serve_resource %s %s\n", get_client_address(client), path);

    int portno;
    unsigned short port;
    char host[10000], address[10000];
    char* server_content;

    if (strcmp(path, "/") == 0) path = "index.html";

    if (strlen(path) > 100) {
        send_400(client_list, client);
        return;
    }


    (void) strncpy( path, "http", 4 );       /* make sure it's lower case */
    if ( sscanf( path, "http://%[^:/]:%d%s", host, &portno, address) == 3 )
        port = (unsigned short) portno;
        else if ( sscanf( path, "http://%[^/]%s", host, address) == 2 )
            port = 80;
        else if ( sscanf( path, "http://%[^:/]:%d", host, &portno) == 2 )
        {
            port = (unsigned short) portno;
            *address = '\0';
        }
        else if ( sscanf( path, "http://%[^/]", host) == 1 )
        {
            port = 80;
            *address = '\0';
        }
        else {
            send_400(client_list, client);
            return ;
        }
    
    char url[10000] = "";
    strcat(url, host);
    strcat(url, address);
    if (!checkIfExisted(hash, q, url, port)){
        bool exist_max_age = false;
        int length;
        char header[10000],cache_control[10000], max_age[10000];
        //char* content = (char*)malloc(MAX_SIZE * sizeof(char));
        server_content = fetch_from_server(client_list, client, host, address, port, &length);
        if (server_content) {
            char* con_index = strstr(server_content, "\r\n\r\n");
            //memcpy(content, con_index + 4, (int)server_content + length - (int)con_index - 4);
            memcpy(header, server_content, (int)con_index - (int)server_content);

            //*(content + (int)server_content + length - (int)con_index + 4) = '\0';
            *(header + (int)con_index - (int)server_content) = '\0';
          
            printf("\nheader:\n%s\n", header);
            char *cache_index_start = strstr(header, "Cache-Control:");
            if(cache_index_start) {
                char *cache_index_end = strstr(cache_index_start, "\r\n");
                if(cache_index_end) {
                    memcpy(cache_control, cache_index_start + 15, (int)cache_index_end - (int)cache_index_start - 15);
                    *(cache_control + (int)cache_index_end - (int)cache_index_start - 15) = '\0';
                }
                else {
                    memcpy(cache_control, cache_index_start + 15, strlen(cache_index_start) - 15);
                    *(cache_control + strlen(cache_index_start) - 15) = '\0';
                }
                printf("cache control: %s\n", cache_control);
                if (sscanf(cache_control, "max-age=%s", max_age) == 1) exist_max_age = true;
                
            }
            send(client->socket, server_content, length, 0);
            if (length > 10 * 1024 && length < MAX_SIZE){
                if(!exist_max_age) strcpy(max_age, "3600");
                printf("max-age is %s\n", max_age);
                time_t now;
                Input* input = createInput(url, port, server_content, atoi(max_age), time(&now), length);
                put_into_cache(q, hash, input);
                
            }
            free(server_content);
            print_cache(q);
        } else{return;}

        
    }else{
        printf("existed!!!!!\n");
        time_t now;
        Input *input = get_from_cache(q, hash, url, port);
        send(client->socket, input->object, input->length, 0);
        print_cache(q);
    }

     drop_client(client_list, client);
}

/*https communication*/
void proxy_https_get_from_client(void* argv){

    // struct client_info **client_list,struct client_info *client,
    //        struct client_info *server
    //
    //===================
    gfcParam* para = (gfcParam *)argv;

    struct client_info **client_list = para->client_list;
    struct client_info *client = para->client;
    struct client_info *server = para->server;
    //===================

    printf("start transfer https data\n");
    int client_fd, server_fd, r, r1;
    r1 = 0;

    char buf[10000], new_buf[10240];

    /* Now forward SSL packets in both directions until done. */
    client_fd = client->socket;
    server_fd = server->socket;

    memset(buf, 0, sizeof(buf));
    r = SSL_read(client->ssl, buf, sizeof(buf));
    if (r <= 0){
        printf("client close socket \n");
        printf("drop client  \n");
        drop_client(client_list, client);
        printf("drop server\n");
        drop_client(client_list, server);
        para->ret = server == client->next ? 0 : -1;
        return;
    } else {
        //update start time
        time_t now;
        client->start_time = time(&now);
        //do filtering
        char match_str[256] = "<p>[^<>]*";
        strcat(match_str, BOLD_WORDS);
        strcat(match_str, "[^<>]*</p>");

        regex_t buf_reg;
        regmatch_t pmatch;
        regcomp(&buf_reg,match_str,REG_EXTENDED);
        if(regexec(&buf_reg,buf,1, &pmatch, 0) == 0){
            printf("match success\n");
            printf("%lld \t %lld", pmatch.rm_so, pmatch.rm_eo);
            char *filter_start = strstr(buf + pmatch.rm_so, BOLD_WORDS);
            if (filter_start) {
                printf("find the target\n");
            
                char new_bold[100];
                memcpy(new_buf, buf, filter_start - buf);
                //strcpy(new_bold, "<strong>");
                //strcpy(new_bold + 8, BOLD_WORDS);
                //strcpy(new_bold + 8 + strlen(BOLD_WORDS), "</strong>");
                //int new_bold_len = 17 + strlen(BOLD_WORDS);
                //printf("new_bold is %s, new_bold_len is %d\n", new_bold, new_bold_len);
                for (int i = 0; i < strlen(BOLD_WORDS); i++) {
                    strcat(new_bold, " ");
                }
                memcpy(new_buf + (filter_start - buf), new_bold, strlen(BOLD_WORDS));
                memcpy(new_buf + strlen(BOLD_WORDS) + (filter_start - buf) , buf + strlen(BOLD_WORDS) + (filter_start - buf), buf + r - filter_start - strlen(BOLD_WORDS));
                
                FILE *receive;
                if((receive = fopen("bold1","wb")) == NULL)
                {
                    perror("fail to read");
                    exit (1) ;
                }
                fwrite(new_buf,r,1,receive);

                fclose(receive);
                if((receive = fopen("bold2","wb")) == NULL)
                {
                    perror("fail to read");
                    exit (1) ;
                }
                fwrite(buf,r ,1,receive);
                fclose(receive);
                /*
                for (int i = 0; i < r; i++) {
                    printf("%c", buf[i]);
                }*/
               r1 = 1;
            }
        }
        regfree(&buf_reg);

        // //do caching
        if (client->is_server && !client->out_of_memory) {
            if (client->message_len + r< MAX_SIZE) {
                if (client->message_len + r > client->cur_size) {//expand
                    
                    char *temp = (char*)malloc(client->cur_size + 10*1024);
                    client->cur_size += 10*1024; 
                    memcpy(temp, client->message, client->message_len);
                    memcpy(temp + client->message_len, buf, r);
                    free(client->message);
                    client->message = temp;
                } else {
                    memcpy(client->message + client->message_len, buf, r);
                }
                client->message_len += r;
            } else {
                if (client->message) free(client->message);
                client->message = NULL;
                client->out_of_memory = true;
            }
            printf("cur size %d, message len %d, r %d", client->cur_size, client->message_len, r);
        }

        if (r1 == 0) r = SSL_write(server->ssl, buf, r);
        else r = SSL_write(server->ssl, new_buf, r);
        //r = SSL_write(server->ssl, buf, r);
        if (r <= 0){
            printf("server close socket\n");
            drop_client(client_list, client);
            drop_client(client_list, server);
            para->ret = server == client->next ? 0 : -1;
            return;
        }
        printf("data transfer finished\n");
    }       
}

/*serve http request*/
void serve_https_resource(void* argv) {

    //===================
    Param* para = (Param *)argv;

    struct client_info **client_list = para->client_list;
    struct client_info *client = para->client;
    const char *path = para->path;
    //===================
    printf("serve_resource %s %s fd is %d\n", get_client_address(client), path, client->socket);
    
    char host[10000];
    int portno,r;
    SOCKET sockfd;
    unsigned short port;

    if (strcmp(path, "/") == 0) path = "index.html";

    if (strlen(path) > 100) {
        send_400(client_list, client);
        return ;
    }
    if ( sscanf( path, "%[^:]:%d", host, &portno ) == 2 )
        port = (unsigned short) portno;
    else if ( sscanf( path, "%s", host ) == 1 )
        port = 443;
    else {
        send_400(client_list, client);
        return ;
    }
    //get socket connected to server
    sockfd = open_client_socket( client_list, client, host, port );
    struct client_info *server = get_client(client_list, -1);

    if (sockfd > 0) {
        client->server_socket = sockfd;
        //treat server socket as client  
        server->socket = sockfd;
        server->is_https = true;
        server->is_server = true;
        strcpy(server->path, path);
        server->server_socket = client->socket;

        server->ctx = SSL_CTX_new(TLS_client_method());
        SSL *ssl = SSL_new(server->ctx);
        server->ssl = ssl;
        if (!ssl) {
            drop_client(client_list, client);
            drop_client(client_list, server);
            fprintf(stderr, "SSL_new() failed.\n");
            para->ret = server == client->next ? 0 : -1;
            return;
        }

        if (!SSL_set_tlsext_host_name(ssl, host)) {
            drop_client(client_list, client);
            drop_client(client_list, server);
            fprintf(stderr, "SSL_set_tlsext_host_name() failed.\n");
            ERR_print_errors_fp(stderr);
            para->ret = server == client->next ? 0 : -1;
            return;
        }

        SSL_set_fd(ssl, sockfd);
        int ret = SSL_connect(ssl);
        if (ret == -1) {
            drop_client(client_list, client);
            drop_client(client_list, server);
            fprintf(stderr, "SSL_connect() failed.\n");
            ERR_print_errors_fp(stderr);
            para->ret = server == client->next ? 0 : -1;
            return;
        }
        //printf("ret is %d, %d,%d, %d\n", ret, SSL_get_error(ssl, ret), SSL_get_fd(ssl), sockfd);
        
        //printf("%s\n",ERR_error_string(ERR_get_error(), NULL)); 

        printf ("SSL/TLS using %s\n", SSL_get_cipher(ssl));
        /*==================================================================*/

        client->ctx = SSL_CTX_new(TLS_server_method());
        if (!client->ctx) {
            fprintf(stderr, "SSL_CTX_new() failed.\n");
            return ;
        }

        get_new_certificate(server->ssl, client->ctx);
        
        
        /*==================================================================*/
        const char *connection_established = "HTTP/1.0 200 Connection established\r\n\r\n";
        /* Return SSL-proxy greeting header. */
        r = send(client->socket, connection_established, strlen(connection_established) + 1, 0);
        if(r <= 0) return ;
        printf("send feedback to client successfully\n");

        client->ssl = SSL_new(client->ctx);
        if (!client->ssl) {
            drop_client(client_list, client);
            drop_client(client_list, server);
            fprintf(stderr, "SSL_new() failed.\n");
            para->ret = server == client->next ? 0 : -1;
            return;
        }

        if (!SSL_check_private_key(client->ssl)){
            fprintf(stderr, "Generate Certificate error\n");
            exit(EXIT_FAILURE);
        }

        SSL_set_fd(client->ssl, client->socket);
        if (SSL_accept(client->ssl) != 1) {
            //SSL_get_error(client->ssl, SSL_accept(...));
            drop_client(client_list, client);
            drop_client(client_list, server);
            printf("SSL accept fail\n");
            ERR_print_errors_fp(stderr); 
            para->ret = server == client->next ? 0 : -1;
            return;
        } else {
            printf("New connection from %s.\n",
                    get_client_address(client));

            printf ("SSL connection using %s\n",
                    SSL_get_cipher(client->ssl));
        }



        char buf[10000];
        char new_buf[10000];
        char *ac_en;
        
        int r = SSL_read(client->ssl,buf,sizeof(buf));

        if (r < 1) {
            drop_client(client_list, client);
            drop_client(client_list, server);
            printf("Unexpected disconnect from %s.\n",
                    get_client_address(client));
            para->ret = server == client->next ? 0 : -1;
            return;
        }else {     
            printf("read successfully\n");
            ac_en = strstr(buf, "Accept-Encoding: ");
            if (ac_en) {
                char *nxt_line = strstr(ac_en, "\n");
                memcpy(new_buf, buf, ac_en - buf);
                memcpy(new_buf + (ac_en - buf), nxt_line + 1, buf + r - nxt_line);
                r -= nxt_line - ac_en;
                // FILE *receive;
                // if((receive = fopen("header.txt","wb")) == NULL)
                // {
                //     perror("fail to read");
                //     exit (1) ;
                // }
                // fwrite(new_buf,r,1,receive);
                // fwrite(buf,r + nxt_line - ac_en,1,receive);
                // fclose(receive);

            }
            printf("read from client%s\n", buf);
        }

        if (ac_en)  r = SSL_write(ssl, new_buf, r);
        else  r = SSL_write(ssl, buf, r);

        if (r < 1) {
            drop_client(client_list, client);
            drop_client(client_list, server);
            printf("Unexpected disconnect from server %s.\n",
                    host);
            para->ret = server == client->next ? 0 : -1;
            return;
        }

       //proxy_https_get_from_client( client_list, client, server);
        //printf("https transfer finished\n");
    }  else {
        drop_client(client_list, client);
        drop_client(client_list, server);
        printf("drop client and server\n");
        para->ret = server == client->next ? 0 : -1;
        return;
    }
}

void send_400(struct client_info **client_list,
        struct client_info *client) {
    const char *c400 = "HTTP/1.1 400 Bad Request\r\n"
        "Connection: close\r\n"
        "Content-Length: 11\r\n\r\nBad Request";
    send(client->socket, c400, strlen(c400), 0);
    drop_client(client_list, client);
}

void send_404(struct client_info **client_list,
        struct client_info *client) {
    const char *c400 = "HTTP/1.1 404 Not Found\r\n"
        "Connection: close\r\n"
        "Content-Length: 13\r\n\r\nUnknown Hosts";
    send(client->socket, c400, strlen(c400), 0);
    drop_client(client_list, client);
}

void send_408(struct client_info **client_list,
        struct client_info *client) {
    const char *c400 = "HTTP/1.1 408 Request Timeout\r\n"
        "Connection: close\r\n"
        "Content-Length: 15\r\n\r\nRequest Timeout";
    send(client->socket, c400, strlen(c400), 0);
    drop_client(client_list, client);
}

void send_500(struct client_info **client_list,
        struct client_info *client) {
    const char *c400 = "HTTP/1.1 500 Internal Error\r\n"
        "Connection: close\r\n"
        "Content-Length: 23\r\n\r\nCould not create socket";
    send(client->socket, c400, strlen(c400), 0);
    drop_client(client_list, client);
}

void send_503(struct client_info **client_list,
        struct client_info *client) {
    const char *c400 = "HTTP/1.1 503 Service Unavailable\r\n"
        "Connection: close\r\n"
        "Content-Length: 18\r\n\r\nConnection refused";
    send(client->socket, c400, strlen(c400), 0);
    printf("connection refuesd\n");
    drop_client(client_list, client);
}

SOCKET server;
struct client_info *client_list;
Hash *hash;
Queue *queue;


void dealWithClient(void* args){
    dwcParam *para = (dwcParam*)args;

    while(1) {

        fd_set reads;
        printf("new select\n");
        reads = wait_on_clients(&client_list, server);

        if (FD_ISSET(server, &reads)) {
            struct client_info *client = get_client(&client_list, -1);

            client->socket = accept(server,
                    (struct sockaddr*) &(client->address),
                    &(client->address_length));

            if (!ISVALIDSOCKET(client->socket)) {
                fprintf(stderr, "accept() failed. (%d)\n",
                        GETSOCKETERRNO());
                return ;
            }

            printf("======================================================================================\n");
            printf("New connection from %s.\n",
                    get_client_address(client));
        }


        struct client_info *client = client_list;
        while(client) {
            struct client_info *next = client->next;
            printf("now the client is %d\n", client->socket);
            //receive first request 
            if (FD_ISSET(client->socket, &reads) && !client->server_socket && !client->is_server) {

                if (MAX_REQUEST_SIZE == client->received) {
                    send_400(&client_list, client);
                    continue;
                }

                int r = recv(client->socket,
                        client->request + client->received,
                        MAX_REQUEST_SIZE - client->received, 0);
                
                printf("%s\n", client->request);

                if (r < 1) {
                    printf("Unexpected disconnect from %s.\n",
                            get_client_address(client));
                    drop_client(&client_list, client);

                } else {
                    client->received += r;
                    client->request[client->received] = 0;

                    char *q = strstr(client->request, "\r\n\r\n");
                    if (q) {
                        *q = 0;

                        if (strncmp("GET ", client->request, 4) && strncmp("CONNECT ", client->request, 8)) {
                            send_400(&client_list, client);
                        } else {
                            if (strncmp("GET ", client->request, 4) == 0){
                                char *path = client->request + 4;
                                char *end_path = strstr(path, " ");
                                if (!end_path) {
                                    send_400(&client_list, client);
                                } else {
                                    *end_path = 0;
                                    printf("======================================================================================\n");
                                    serve_http_resource(&client_list, client, path, hash, queue);
                                }
                            } else {//connect
                                char *path = client->request + 8;
                                char *end_path = strstr(path, " ");
                                if (!end_path) {
                                    send_400(&client_list, client);
                                } else {
                                    *end_path = 0;
                                    printf("======================================================================================\n");
                                    client->is_https = true;
                                    

                                    //==========================

                                    pthread_t thread;
                                    Param *para = (Param*)malloc(sizeof(Param)); // warp param into a sturct
                                    para->client_list = &client_list;
                                    para->client = client;
                                    para->path = path;
                                    para->ret = 1;
                                    pthread_create(&thread,NULL,(void *)serve_https_resource,(void *)para); // create a thread
                                    pthread_join(thread,NULL);
                                    if (para->ret == 0) {
                                        if(next->next) client = next->next;
                                        else client = next;
                                        free(para);
                                        para = NULL;
                                        continue;   
                                    }
                                    free(para);
                                    para = NULL;
                                    //==========================   

                                }
                            }
                        }
                    } //if (q)
                }
            } else if (FD_ISSET(client->socket, &reads) && client->is_server && client->server_socket){//read data from server
                if(client->is_https) {
                    //client is actual server
                    printf("get data from server %d, %s, client is %d\n", client->socket, client->path, client->server_socket);

                    //==========================
                    pthread_t thread;
                    gfcParam *para = (gfcParam*)malloc(sizeof(gfcParam)); // warp param into a sturct
                    para->client_list = &client_list;
                    para->client = client;
                    para->server = get_client(&client_list, client->server_socket);
                    para->ret = 1;
                    pthread_create(&thread,NULL,(void *)proxy_https_get_from_client,(void *)para); // create a thread
                    pthread_join(thread,NULL);
                    if(para->ret == 0) {
                        printf("1now the client is %d\n", client->socket);
                        if(next->next) client = next->next;
                        else client = next;
                        free(para);
                        para = NULL;
                        continue;
                    } 
                    free(para);
                    para = NULL;
                    //==========================
                } else{

                }

            }else if (FD_ISSET(client->socket, &reads) && !client->is_server && client->server_socket) {//send data to server
                if (client->is_https) {
                    printf("get data from client %d\n", client->socket);
                    //==========================
                    pthread_t thread;
                    gfcParam *para = (gfcParam*)malloc(sizeof(gfcParam)); // warp param into a sturct
                    para->client_list = &client_list;
                    para->client = client;
                    para->server = get_client(&client_list, client->server_socket);
                    para->ret = 1;
                    pthread_create(&thread,NULL,(void *)proxy_https_get_from_client,(void *)para); // create a thread
                    pthread_join(thread,NULL);
                    if(para->ret == 0) {
                        printf("2now the client is %d\n", client->socket);
                        if(next->next) client = next->next;
                        else client = next;
                        free(para);
                        para = NULL;
                        continue;
                    } 
                    free(para);
                    para = NULL;
                    //==========================

                } else{

                }
            }
            client = next;
            
        }

    } //while(1)
}

int main() {

    pthread_t tid;
    pthread_mutex_init(&mutex,NULL);

    server = create_socket(0, "8080");

    client_list = 0;

    hash = createHash(100);
    queue = createQueue(100);

    /*===============================SSL===========================================*/

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    
    /*==========================================================================*/

    dwcParam *para= (dwcParam*)malloc(sizeof(dwcParam));

    while(1) {
        //pthread_t thread;
        fd_set reads;
        reads = wait_on_clients(&client_list, server);
        para->reads = reads;

        pthread_create(&tid,NULL,(void*)dealWithClient,(void*)para);
        pthread_join(tid,NULL);
        //dealWithClient((void*)para);


    } //while(1)


    printf("\nClosing socket...\n");
    CLOSESOCKET(server);

    printf("Finished.\n");
    return 0;
}
