#include <glib.h>
#include <librdkafka/rdkafka.h>
#include "common.cc"

using namespace std;

// KAFKA
// can store and read as many times as you want
// One topic (chat messages), multiple partitions
// Partitions based on user ID
// One consumer and producer per backend server
// (because consumer/producer instance is not lightweight)

// CONSUMER
// Each consumer can subscribe to the topic+partition pair based on frontend users
// Consumer can then send the message to the frontend users based on user IP address
// Dynamically subscribe to new partitions based on incoming frontend connections

// partition can only be consumed by one consumer within the consumer group
// so for each backend server, have one consumer group for each topic+partition pair

// consumer client nees to long poll to stay connected to the broker and receive message
// it also sends heartbeat this way
unordered_set<int> subscribed_partitions;			 // set of partitions that server is subscribed to
unordered_map<int, string> partition_consumer_group; // partition: consumer group

// PRODUCER
// Each producer just send the message to the topic, with KEY as the frontend user ID

// SETTINGS
bool print_debug = false;
volative sig_atomic_t shutdown_server = 0;
int frontend_port;
string server_ip; // current BE server IP and port
int FRONTEND_MAX_CONNECTIONS = 100;
int partition_count = 3; // number of partition within topic

// SHARED RESOURCES
unordered_map<string, int> user_fd;				   // user ID: FD
unordered_map<string, vector<string>> group_users; // groupID: vector of user IDs
queue<string> message_queue;					   // message queue to send to producer
mutex message_queue_mutex;

void signal_handler(int signo)
{
	if (signo == SIGINT)
	{
		shutdown();
	}
}

void shutdown()
{
	shutdown_server = 1;
}

/* Optional per-message delivery callback (triggered by poll() or flush())
 * when a message has been successfully delivered or permanently
 * failed delivery (after retries).
 */
static void dr_msg_cb(rd_kafka_t *kafka_handle,
					  const rd_kafka_message_t *rkmessage,
					  void *opaque)
{
	if (rkmessage->err)
	{
		g_error("Message delivery failed: %s", rd_kafka_err2str(rkmessage->err));
	}
}

void accept_frontend_connections()
{
	int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
	{
		cerr << "Cannot open socket" << endl;
		shutdown_server = 1;
		return;
	}

	// ensure that after shutting down the server, you can bind to the same port right away
	int option = 1;
	setsockopt(frontendfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(option));
	struct sockaddr_in serverAddr;
	bzero(&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(frontend_port);
	serverAddr.sin_addr.s_addr = inet_addr(server_ip.c_str());
	if (::bind(listen_fd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
	{
		cerr << "Failed to bind to frontend port" << endl;
		shutdown_server = 1;
		return;
	}

	if (listen(listen_fd, FRONTEND_MAX_CONNECTIONS) < 0)
	{
		cerr << "Failed to listen for frontend connections" << endl;
		shutdown_server = 1;
		return;
	}

	vector<int> frontend_connections;
	vector<int> closed_connections;
	while (shutdown_server == 0)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(frontend_fd, &r);
		int max_fd = frontendfd;

		// handle new connections
		for (int fd : frontend_connections)
		{
			// check if valid or closed
			int flag = fcntl(fd, F_GETFD);
			if (flag == -1)
			{
				close(fd);
				closed_connections.push_back(fd);
				continue;
			}
			else
			{
				FD_SET(fd, &r);
				max_fd = max(max_fd, fd);
			}
		}

		// remove closed connections
		for (int fd : closed_connections)
		{
			frontend_connections.erase(remove(frontend_connections.begin(), frontend_connections.end(), fd), frontend_connections.end());
		}

		int ret = select(max_fd + 1, &r, NULL, NULL, NULL);
		if (ret < 0)
		{
			shutdown();
			return;
		}

		// if new client
		if (FD_ISSET(frontend_fd, &r))
		{
			// accept the next incoming connection
			struct sockaddr_in src;
			socklen_t srclen = sizeof(src);
			int connfd = accept(frontendfd, (struct sockaddr *)&src, &srclen);
			string client_ip_port = get_address(src);
			if (connfd < 0)
			{
				cerr << "Error with accept function in frontend thread" << endl;
				continue;
			}
			print_debug(debug_print, "New frontend connection FD: %d\n", connfd);
			print_debug(debug_print, "New frontend client IP: %s\n", client_ip_port.c_str());

			// get userID from client

			// subscribe to the partition for this user
			// create a consumer group
			int partition = get_partition(user_id)
				subscribe_to_partition(user_id, partition);

			// send greeting message
			string greeting = "+OK Server ready\r\n";
			do_write_backend(connfd, greeting.data(), greeting.size());
			frontend_connections.push_back(connfd);
		}

		// handle messages from users, put into message queue
		for (int fd : frontend_connections)
		{
			if (FD_ISSET(fd, &r))
			{
				string message = do_read_message(fd, "\r\n");
				// send message to producer
				lockguard<mutex> lock(message_queue_mutex);
				message_queue.push(message);
			}
		}
	}
	// close all connections
	for (int fd : frontend_connections)
	{
		close(fd);
		print_debug(debug_print, "Closed frontend connection FD: %d\n", fd);
	}
	shutdown_server();
}

void get_partition(string user_id)
{
	return partition_count % user_id[0];
}

void subscribe_to_partition(string user_id, int partition)
{
	// check if already subscribed
	if (subscribed_partitions.find(partition) != subscribed_partitions.end())
	{
		return;
	}
	// subscribe to the partition
	subscribed_partitions.insert(partition);

	// create a consumer group for this server for this partition
	partition_consumer_group[partition] = user_id;

	// subscribe to this partition
}

void consume_messages()
{
}

void produce_message()
{
	// Connect to producer
	string broker = "localhost:9092";
	const char *topic = "chat_messages";
	rd_kafka_t *producer;

	// Set configs
	char errstr[512];
	rd_kafka_conf_t *conf;
	conf = rd_kafka_conf_new();
	set_config(conf, "bootstrap.servers", broker.c_str());
	set_config(conf, "sasl.mechanisms", "PLAIN");
	set_config(conf, "acks", "all");

	// Install a delivery-error callback
	rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);

	// Create the Producer instance
	producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
	if (!producer)
	{
		g_error("Failed to create new producer: %s", errstr);
		return 1;
	}

	// Configuration object is now owned, and freed, by the rd_kafka_t instance
	conf = NULL;

	while (shutdown_server == 0)
	{
		lock_guard<mutex> lock(message_queue_mutex);
		if (message_queue.empty())
		{
			continue;
		}
		string message = message_queue.front(); // sender:receiver:message
		err = rd_kafka_producev(producer,
								RD_KAFKA_V_TOPIC(topic),
								RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
								RD_KAFKA_V_KEY((void *)key, key_len),
								RD_KAFKA_V_VALUE((void *)value, value_len),
								RD_KAFKA_V_OPAQUE(NULL),
								RD_KAFKA_V_END);
	}

	// Produce data by selecting random values from these lists.
	int message_count = 10;
	const char *user_ids[6] = {"eabara", "jsmith", "sgarcia", "jbernard", "htanaka", "awalther"};
	const char *products[5] = {"book", "alarm clock", "t-shirts", "gift card", "batteries"};

	for (int i = 0; i < message_count; i++)
	{
		const char *key = user_ids[random() % ARR_SIZE(user_ids)];
		const char *value = products[random() % ARR_SIZE(products)];
		size_t key_len = strlen(key);
		size_t value_len = strlen(value);

		rd_kafka_resp_err_t err;

		err = rd_kafka_producev(producer,
								RD_KAFKA_V_TOPIC(topic),
								RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
								RD_KAFKA_V_KEY((void *)key, key_len),
								RD_KAFKA_V_VALUE((void *)value, value_len),
								RD_KAFKA_V_OPAQUE(NULL),
								RD_KAFKA_V_END);

		if (err)
		{
			g_error("Failed to produce to topic %s: %s", topic, rd_kafka_err2str(err));
			return 1;
		}
		else
		{
			g_message("Produced event to topic %s: key = %12s value = %12s", topic, key, value);
		}

		rd_kafka_poll(producer, 0);
	}

	// Block until the messages are all sent.
	g_message("Flushing final messages..");
	rd_kafka_flush(producer, 10 * 1000);

	if (rd_kafka_outq_len(producer) > 0)
	{
		g_error("%d message(s) were not delivered", rd_kafka_outq_len(producer));
	}

	g_message("%d events were produced to topic %s.", message_count, topic);

	rd_kafka_destroy(producer);

	return 0;
}

// command args: ./backendserver -v IP:port
int main(int argc, char **argv)
{
	// Settings
	signal(SIGINT, signal_handler);

	if (argc < 2)
	{
		cerr << "Incorrect usage" << endl;
		return EXIT_FAILURE:
	}

	int opt;
	while ((opt = getopt(argc, argv, "v")) != -1)
	{
		switch (opt)
		{
		case 'v':
			print_debug = true;
			break;
		default:
			cerr << "Incorrect usage" << endl;
			return EXIT_FAILURE;
		}
	}

	string IP_port = argv[optind];
	vector<string> IP_port_vector = split(IP_port, ":");
	server_IP = IP_port_vector.at(0);
	frontend_port = stoi(IP_port_vector.at(1));

	thread producer_thread(produce_messages);
	thread consumer_thread(consume_messages);
	thread frontend_thread(accept_frontend_connections);

	producer_thread.join();
	consumer_thread.join();
	frontend_thread.join();

	return EXIT_SUCCESS;
}