#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <netdb.h>
#include <resolv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>

#define BUFSIZE 1024
#define TIMEOUT 300


#define SERVER_SOCKET_ERROR -1
#define SERVER_SETSOCKOPT_ERROR -2
#define SERVER_BIND_ERROR -3
#define SERVER_LISTEN_ERROR -4
#define CLIENT_SOCKET_ERROR -5
#define CLIENT_RESOLVE_ERROR -6
#define CLIENT_CONNECT_ERROR -7
#define CREATE_PIPE_ERROR -8
#define BROKEN_PIPE_ERROR -9
#define HEADER_BUFFER_FULL -10
#define BAD_HTTP_PROTOCOL -11

void sigchld_handler(int signal);

int create_server_socket(int port);

void handle(void *args);


int create_connection(char *hostname, int portno);

void error(char *msg);

void proxy_ssl(void* args);

void proxy_http(void* args);

typedef struct thrArgs {
    int newsock;
    struct sockaddr_in client_addr;

} Param;


typedef struct thrArgs2
{
    int server;
   int client;

}Param2;



int main(int argc, char **argv) {

    int portno = 8080; /* port to listen on */
    char buf[BUFSIZE]; /* message buffer */

    pthread_mutex_t mutex;



    //fd_set tmp_set, master_set;

    //int fdmax;
    int parentfd = create_server_socket(portno); /* parent socket */
    int childfd; /* child socket */
    //int clientlen;



    signal(SIGCHLD, sigchld_handler);
    pthread_t tid;
    pthread_mutex_init(&mutex, NULL);
    Param *para = (Param *) malloc(sizeof(Param));

    while (1) {
        struct sockaddr_in clientaddr; /* client addr */
        socklen_t addrlen = sizeof(clientaddr);
        childfd = accept(parentfd, (struct sockaddr *) &clientaddr, &addrlen);


        para->client_addr = clientaddr;
        para->newsock = childfd;


        if (childfd > 0) {
            pthread_create(&tid, NULL, (void*) handle, (void*) para);
            pthread_detach(tid);
        }

        //close(childfd);


    }


    close(parentfd);
    return (0);

}


int create_server_socket(int port) {
    int server_sock, optval;
    struct sockaddr_in server_addr;

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return SERVER_SOCKET_ERROR;
    }

    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        return SERVER_SETSOCKOPT_ERROR;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
        return SERVER_BIND_ERROR;
    }

    if (listen(server_sock, 20) < 0) {
        return SERVER_LISTEN_ERROR;
    }

    return server_sock;
}


void sigchld_handler(int signal) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}


void error(char *msg) {
    perror(msg);
}


void handle(void *args) {

    Param *para = (Param *) args;
    int newsock = para->newsock;
    struct sockaddr_in clientaddr = para->client_addr;

    int is_ssl = 0;

    int n = 0;
    char request[BUFSIZE]; // http request from client
    bzero(request, BUFSIZE);


    n = read(newsock, request, BUFSIZE);


    if (n <= 0) // client close the socket
    {


        close(newsock);


    } else // deal with message
    {
        printf("bytes: %d \n%s \n", n, request);

        char method[BUFSIZE], url[BUFSIZE], protocol[BUFSIZE], path[BUFSIZE], host[BUFSIZE];
        int iport, ssl;
        unsigned short port;

        //=======================parse==================================
        if (sscanf(request, "%[^ ] %[^ ] %[^\r\n]", method, url, protocol) != 3)
            printf("error prase request from client\n");


        if (strncasecmp(url, "http://", 7) == 0) {
            (void) strncpy(url, "http", 4);    /* make sure it's lower case */
            if (sscanf(url, "http://%[^:/]:%d%s", host, &iport, path) == 3)
                port = (unsigned short) iport;
            else if (sscanf(url, "http://%[^/]%s", host, path) == 2)
                port = 80;
            else if (sscanf(url, "http://%[^:/]:%d", host, &iport) == 2) {
                port = (unsigned short) iport;
                *path = '\0';
            } else if (sscanf(url, "http://%[^/]", host) == 1) {
                port = 80;
                *path = '\0';
            } else
                printf("error \n");
            ssl = 0;
        } else if (strcmp(method, "CONNECT") == 0) {
            if (sscanf(url, "%[^:]:%d", host, &iport) == 2)
                port = (unsigned short) iport;
            else if (sscanf(url, "%s", host) == 1)
                port = 443;
            else
                printf("error \n");
            ssl = 1;
        } else
            printf("error \n");
        //=======================parse==================================


        printf("%s %s %s %d\n", method, protocol, host, port);

        int server_sock = create_connection(host, port);


        if (ssl) {

            char *buff = "HTTP/1.1 200 Connection established\r\n\r\n";


            write(newsock, buff, strlen(buff));


            pthread_t thread;
            Param2 *para = (Param2*)malloc(sizeof(Param2)); // warp param into a sturct
            para->server = server_sock;
            para->client = newsock;
            pthread_create(&thread,NULL,(void *)proxy_ssl,(void *)para); // create a thread
            pthread_join(thread,NULL);
            free(para);
            para = NULL;



            //proxy_ssl(server_sock, newsock);


        } else {


            write(server_sock, request, strlen(request));

            pthread_t thread;
            Param2 *para = (Param2*)malloc(sizeof(Param2)); // warp param into a sturct
            para->server = server_sock;
            para->client = newsock;
            pthread_create(&thread,NULL,(void *)proxy_http,(void *)para); // create a thread
            pthread_join(thread,NULL);
            free(para);
            para = NULL;


            //proxy_http(server_sock, newsock);

        }


    }


    //close(server_sock);




}


int create_connection(char *hostname, int portno) {

    int sockfd, n;
    struct sockaddr_in serveraddr;
    struct hostent *server;


    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host as %s\n", hostname);
        //exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *) server->h_addr,
          (char *) &serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    /* connect: create a connection with the server */
    if (connect(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
        error("ERROR connecting");



    // char buf[BUFSIZE];
    // sprintf(buf,"%s 200 Connection established\r\n\r\n",protocol);

    return sockfd;


}


void proxy_ssl(void* args) {

    Param2* para = (Param2 *)args;

    int server = para->server;
    int client = para->client;


    struct timeval timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;
    fd_set fdset;
    int maxp1, r;
    char buf[10000];

    if (client >= server)
        maxp1 = client + 1;
    else
        maxp1 = server + 1;


    for (;;) {
        FD_ZERO(&fdset);
        FD_SET(client, &fdset);
        FD_SET(server, &fdset);
        r = select(maxp1, &fdset, (fd_set *) 0, (fd_set *) 0, &timeout);
        if (r == 0) {
            printf("error\n");
            break;
        } else if (FD_ISSET(client, &fdset)) {
            r = read(client, buf, sizeof(buf));
            if (r <= 0)
                break;
            r = write(server, buf, r);
            if (r <= 0)
                break;
        } else if (FD_ISSET(server, &fdset)) {
            r = read(server, buf, sizeof(buf));
            if (r <= 0)
                break;
            r = write(client, buf, r);

            if (r <= 0)
                break;
        }
    }


}


void proxy_http(void* args) {

    Param2* para = (Param2 *)args;

    int server = para->server;
    int client = para->client;


    struct timeval timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;
    fd_set fdset;
    int maxp1, r;
    char buf[10000];

    if (client >= server)
        maxp1 = client + 1;
    else
        maxp1 = server + 1;


    for (;;) {
        FD_ZERO(&fdset);
        FD_SET(client, &fdset);
        FD_SET(server, &fdset);
        r = select(maxp1, &fdset, (fd_set *) 0, (fd_set *) 0, &timeout);
        if (r == 0) {
            printf("error\n");
            break;
        } else if (FD_ISSET(client, &fdset)) {
            r = read(client, buf, sizeof(buf));
            if (r <= 0)
                break;
            r = write(server, buf, r);
            if (r <= 0)
                break;
        } else if (FD_ISSET(server, &fdset)) {
            r = read(server, buf, sizeof(buf));
            if (r <= 0)
                break;
            r = write(client, buf, r);

            if (r <= 0)
                break;
        }
    }


}
