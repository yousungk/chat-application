#ifndef EXTERN_H
#define EXTERN_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <csignal>
#include <unordered_map>
#include <queue>

using namespace std;

extern string brokers;
class KafkaProducer;
class KafkaConsumer;

// CONFIGS
extern string server_ID; // unique ID for each backend server, used to create consumer group for kafka
extern int PARTITION_COUNT;
extern bool debug_print;
extern volatile sig_atomic_t shutdown_server;
extern int server_port;
extern string server_ip; // current BE server IP and port
extern int FRONTEND_MAX_CONNECTIONS;

// RESOURCES
extern unordered_map<string, string> user_frontendID;		   // user ID: frontend server ID
extern unordered_map<string, vector<string>> frontendID_users; // frontend server ID : list of users
extern unordered_map<string, int> frontendID_fd;			   // frontend server ID : fd
extern unordered_map<int, string> fd_frontendID;
extern unordered_map<string, vector<string>> group_users; // groupID: vector of user IDs
extern vector<int> frontend_connections;
extern vector<int> closed_connections;
extern queue<pair<string, string>> message_queue;
extern mutex message_queue_mutex;
extern condition_variable message_queue_cv;
extern int listen_fd;

// BROKER RESOURCES
extern queue<string> message_to_send;
extern mutex message_lock;

// CASSANDRA RESOURCES
extern queue<string> message_to_save;

#endif