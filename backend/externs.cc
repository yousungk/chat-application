#include <externs.h>
#include "consumer.h"
#include "producer.h"

using namespace std;

// create producer and consumer
string brokers = "localhost:9092,localhost:9094,localhost:9096"; // Kafka broker address

// CONFIGS
string server_ID;
int PARTITION_COUNT = 3; // number of partition within topic
bool debug_print = true;
volatile sig_atomic_t shutdown_server = 0;
int server_port;
string server_ip; // current BE server IP and port
int FRONTEND_MAX_CONNECTIONS = 100;

// RESOURCES
unordered_map<string, string> user_frontendID;			// user ID: frontend server ID
unordered_map<string, vector<string>> frontendID_users; // frontend server ID : list of users
unordered_map<string, int>
	frontendID_fd;								   // frontend server ID : fd
unordered_map<int, string> fd_frontendID;		   // fd : frontend server ID
unordered_map<string, vector<string>> group_users; // groupID: vector of user IDs
vector<int> frontend_connections;
vector<int> closed_connections;
queue<pair<string, string>> message_queue;
mutex message_queue_mutex;
condition_variable message_queue_cv;
int listen_fd;

// BROKER RESOURCES
queue<string> message_to_send;
mutex message_lock;

// CASSANDRA RESOURCES
queue<string> message_to_save;