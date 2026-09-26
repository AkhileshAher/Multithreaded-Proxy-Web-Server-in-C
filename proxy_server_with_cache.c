#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 4096
#define MAX_ELEMENT_SIZE (10 * (1 << 10))
#define MAX_SIZE (200 * (1 << 20))

typedef struct cache_element cache_element;

struct cache_element {
    char *data;
    int len;
    char *url;
    time_t lru_time_track;
    cache_element *next;
};

cache_element *head = NULL;
int cache_size = 0;
int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t semphore;
pthread_mutex_t lock;

cache_element *find(char *url);
int add_cache_element(char *data, int size, char *url);
void remove_cache_element(void);

int connectRemoteServer(char *host_addr, int port_num) {
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (remoteSocket < 0) {
        perror("Error creating socket");
        return -1;
    }

    struct hostent *host = gethostbyname(host_addr);
    if (host == NULL) {
        fprintf(stderr, "No such host exists\n");
        close(remoteSocket);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    memcpy(&server_addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (connect(remoteSocket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error connecting");
        close(remoteSocket);
        return -1;
    }

    return remoteSocket;
}

int checkHTTPVersion(char *msg) {
    if (msg == NULL) return -1;
    if (strncmp(msg, "HTTP/1.1", 8) == 0) return 1;
    if (strncmp(msg, "HTTP/1.0", 8) == 0) return 1;
    return -1;
}

int handle_request(int clientSocketId, struct ParsedRequest *request, char *tempReq) {
    char *buf = malloc(MAX_BYTES);
    if (buf == NULL) return -1;

    memset(buf, 0, MAX_BYTES);
    strcpy(buf, "GET ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);

    if (ParsedHeader_set(request, "Connection", "close") < 0) {
        printf("Failed to set Connection header\n");
    }

    if (ParsedHeader_get(request, "Host") == NULL) {
        if (ParsedHeader_set(request, "Host", request->host) < 0) {
            printf("Failed to set Host header\n");
        }
    }

    if (ParsedRequest_unparse_headers(request, buf + len, MAX_BYTES - len) < 0) {
        printf("Failed to unparse headers\n");
        free(buf);
        return -1;
    }

    int server_port = request->port ? atoi(request->port) : 80;
    int remoteSocketId = connectRemoteServer(request->host, server_port);

    if (remoteSocketId < 0) {
        free(buf);
        return -1;
    }

    int bytes_received = recv(remoteSocketId, buf, MAX_BYTES - 1, 0);

    if (send(remoteSocketId, buf, strlen(buf), 0) < 0) {
        perror("Error sending request");
        close(remoteSocketId);
        free(buf);
        return -1;
    }

    char *temp_buffer = malloc(MAX_BYTES);
    if (temp_buffer == NULL) {
        close(remoteSocketId);
        free(buf);
        return -1;
    }

    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    while (bytes_received > 0) {
        if (send(clientSocketId, buf, bytes_received, 0) < 0) {
            perror("Error sending data");
            break;
        }

        if (temp_buffer_index + bytes_received + 1 > temp_buffer_size) {
            temp_buffer_size += MAX_BYTES;
            char *new_buffer = realloc(temp_buffer, temp_buffer_size);

            if (new_buffer == NULL) {
                free(temp_buffer);
                close(remoteSocketId);
                free(buf);
                return -1;
            }

            temp_buffer = new_buffer;
        }

        memcpy(temp_buffer + temp_buffer_index, buf, bytes_received);
        temp_buffer_index += bytes_received;

        memset(buf, 0, MAX_BYTES);
        bytes_received = recv(remoteSocketId, buf, MAX_BYTES - 1, 0);
    }

    temp_buffer[temp_buffer_index] = '\0';

    if (temp_buffer_index > 0) {
        add_cache_element(temp_buffer, temp_buffer_index, tempReq);
    }

    free(temp_buffer);
    free(buf);
    close(remoteSocketId);

    return 0;
}

int sendErrorMessage(int socket, int status_code) {
    char str[1024];
    const char *message;

    switch (status_code) {
        case 400: message = "400 Bad Request"; break;
        case 403: message = "403 Forbidden"; break;
        case 404: message = "404 Not Found"; break;
        case 500: message = "500 Internal Server Error"; break;
        case 501: message = "501 Not Implemented"; break;
        case 505: message = "505 HTTP Version Not Supported"; break;
        default: return -1;
    }

    snprintf(str, sizeof(str),
        "HTTP/1.1 %s\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
        "<html><body><h1>%s</h1></body></html>",
        message, message);

    send(socket, str, strlen(str), 0);
    return 1;
}

void *thread_fn(void *socketNew) {
    int p;
    sem_wait(&semphore);
    sem_getvalue(&semphore, &p);

    int socket = *((int *)socketNew);
    char *buffer = calloc(MAX_BYTES, 1);

    if (buffer == NULL) {
        sem_post(&semphore);
        return NULL;
    }

    int bytes_received = recv(socket, buffer, MAX_BYTES - 1, 0);

    while (bytes_received > 0 && strstr(buffer, "\r\n\r\n") == NULL) {
        int len = strlen(buffer);
        if (len >= MAX_BYTES - 1) break;
        bytes_received = recv(socket, buffer + len, MAX_BYTES - len - 1, 0);
    }

    char *tempReq = malloc(strlen(buffer) + 1);

    if (tempReq == NULL) {
        free(buffer);
        close(socket);
        sem_post(&semphore);
        return NULL;
    }

    strcpy(tempReq, buffer);

    cache_element *temp = find(tempReq);

    if (temp != NULL) {
        int pos = 0;

        while (pos < temp->len) {
            int chunk = temp->len - pos;
            if (chunk > MAX_BYTES) chunk = MAX_BYTES;

            int sent = send(socket, temp->data + pos, chunk, 0);
            if (sent <= 0) break;

            pos += sent;
        }

        printf("Data retrieved from cache\n");
    } else if (bytes_received > 0) {
        struct ParsedRequest *request = ParsedRequest_create();

        if (request == NULL) {
            sendErrorMessage(socket, 500);
        } else if (ParsedRequest_parse(request, buffer, strlen(buffer)) < 0) {
            printf("Parsing failed\n");
            sendErrorMessage(socket, 400);
        } else if (!strcmp(request->method, "GET")) {
            if (request->host && request->path && checkHTTPVersion(request->version) == 1) {
                if (handle_request(socket, request, tempReq) < 0) {
                    sendErrorMessage(socket, 500);
                }
            } else {
                sendErrorMessage(socket, 400);
            }
        } else {
            sendErrorMessage(socket, 501);
        }

        ParsedRequest_destroy(request);
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    free(tempReq);

    sem_post(&semphore);
    sem_getvalue(&semphore, &p);

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }

    port_number = atoi(argv[1]);

    sem_init(&semphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

    if (proxy_socketId < 0) {
        perror("Failed to create socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(proxy_socketId);
        return 1;
    }

    if (listen(proxy_socketId, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        close(proxy_socketId);
        return 1;
    }

    printf("Starting Proxy server at port : %d\n", port_number);
    printf("Proxy server is listening...\n");

    int i = 0;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr, &client_len);

        if (client_socketId < 0) {
            perror("Accept failed");
            continue;
        }

        int *socketId = malloc(sizeof(int));

        if (socketId == NULL) {
            close(client_socketId);
            continue;
        }

        *socketId = client_socketId;

        if (pthread_create(&tid[i], NULL, thread_fn, socketId) != 0) {
            perror("pthread_create failed");
            close(client_socketId);
            free(socketId);
            continue;
        }

        pthread_detach(tid[i]);

        i++;

        if (i >= MAX_CLIENTS) i = 0;
    }

    close(proxy_socketId);
    return 0;
}

cache_element *find(char *url) {
    cache_element *site = NULL;

    pthread_mutex_lock(&lock);

    site = head;

    while (site != NULL) {
        if (strcmp(site->url, url) == 0) {
            site->lru_time_track = time(NULL);
            break;
        }

        site = site->next;
    }

    pthread_mutex_unlock(&lock);

    return site;
}

int add_cache_element(char *data, int size, char *url) {
    pthread_mutex_lock(&lock);

    int element_size = size + 1 + strlen(url) + sizeof(cache_element);

    if (element_size > MAX_ELEMENT_SIZE) {
        pthread_mutex_unlock(&lock);
        return 0;
    }

    while (cache_size + element_size > MAX_SIZE) {
        pthread_mutex_unlock(&lock);
        remove_cache_element();
        pthread_mutex_lock(&lock);
    }

    cache_element *element = malloc(sizeof(cache_element));

    if (element == NULL) {
        pthread_mutex_unlock(&lock);
        return 0;
    }

    element->data = malloc(size + 1);
    element->url = malloc(strlen(url) + 1);

    if (element->data == NULL || element->url == NULL) {
        free(element->data);
        free(element->url);
        free(element);
        pthread_mutex_unlock(&lock);
        return 0;
    }

    memcpy(element->data, data, size);
    element->data[size] = '\0';

    strcpy(element->url, url);

    element->len = size;
    element->lru_time_track = time(NULL);
    element->next = head;
    head = element;

    cache_size += element_size;

    pthread_mutex_unlock(&lock);

    return 1;
}

void remove_cache_element(void) {
    pthread_mutex_lock(&lock);

    if (head == NULL) {
        pthread_mutex_unlock(&lock);
        return;
    }

    cache_element *current = head;
    cache_element *previous = NULL;
    cache_element *lru = head;
    cache_element *lru_previous = NULL;

    while (current != NULL) {
        if (current->lru_time_track < lru->lru_time_track) {
            lru = current;
            lru_previous = previous;
        }

        previous = current;
        current = current->next;
    }

    if (lru_previous == NULL) {
        head = lru->next;
    } else {
        lru_previous->next = lru->next;
    }

    int element_size = lru->len + 1 + strlen(lru->url) + sizeof(cache_element);
    cache_size -= element_size;

    free(lru->data);
    free(lru->url);
    free(lru);

    pthread_mutex_unlock(&lock);
}