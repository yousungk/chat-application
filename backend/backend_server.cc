#include <glib.h>
#include <librdkafka/rdkafka.h>
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
#include <fcntl.h>
#include <mutex>
#include <thread>
#include "externs.h"
#include "producer.h"
#include "consumer.h"
#include "cassandra_client.h"
#include "utils.h"

using namespace std;

// TODO
// 1. deploy using AWS and docker

// frontend connects to server, go to coordinator to get the backend server (load balancer)
// multiple backend servers
// multiple brokers - 3 (partitions across brokers, replicated across brokers)
// broker1 : Partition 0(leader), Partition 1(replica).
// broker2 : Partition 1(leader), Partition 2(replica).
// broker3 : Partition 2(leader), Partition 0(replica).

// producer connects to the broker that is the leader for the partition and sends message
// producer first connect to any broker in Kafka cluster, get metadata about the topic, including list of partitions, leader in each partition, replicas

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

// consumer client needs to long poll to stay connected to the broker and receive message
// it also sends heartbeat this way

// PRODUCER
// Each producer just send the message to the topic, with KEY as the frontend user ID

KafkaConsumer *consumer;
KafkaProducer *producer;
Cassandra *cassandra;

void signal_handler(int signo)
{
	if (signo == SIGINT)
	{
		shutdown_server = 1;
	}
}

void shutdown()
{
	print_debug(debug_print, "Shutting down server\n");
	shutdown_server = 1;
	for (int fd : frontend_connections)
	{
		close(fd);
	}
	close(listen_fd);
	message_queue_cv.notify_one();
}

// thread for sending out messages
// backend needs to connect to
void send_message_to_frontend()
{
	print_debug(debug_print, "Sending message to frontend thread started\n");
	// get message from queue
	while (shutdown_server == 0)
	{
		lock_guard<mutex> lock(message_lock);
		if (!message_to_send.empty())
		{
			string message = message_to_send.front();
			message.append("\n");
			message_to_send.pop();
			print_debug(debug_print, "Got message to send to frontend: %s\n", message.c_str());
			// SENDERID RECIPIENTID MESSAGE
			vector<string> clean_message = split(message, ' ');
			string recipient = clean_message.at(1);
			string frontend_server = user_frontendID[recipient];
			int to_send_fd = frontendID_fd[frontend_server];
			do_write(to_send_fd, message.c_str(), message.size());
			print_debug(debug_print, "Sent message to %s fd %d: %s\n", recipient.c_str(), to_send_fd, message.c_str());
		}
	}
	print_debug(debug_print, "Sending message to frontend thread closed\n");
}

// thread for saving chat messages to Cassandra
void save_message_to_cassandra()
{
	print_debug(debug_print, "Saving message to Cassandra thread started\n");
	while (shutdown_server == 0)
	{
		if (!message_to_save.empty())
		{
			string c_message = message_to_save.front();
			message_to_save.pop(); // Remove the message from the queue
			// parse the message
			// SENDER RECIPIENT MESSAGE

			// Parse the message
			istringstream iss(c_message);
			string timestamp, sender, recipient, message;

			if (getline(iss, sender, ' ') &&
				getline(iss, recipient, ' ') &&
				getline(iss, message))
			{
				// Successfully parsed the message
				cassandra->save_message_to_db(sender, recipient, sender, message);
				print_debug(debug_print, "Got message to save to Cassandra: %s\n", c_message.c_str());
			}
			else
			{
				// Handle parsing error
				print_debug(debug_print, "Failed to parse message: %s\n", c_message.c_str());
			}
		}
	}
}

// thread function to send message to Kafka broker
// using producer client
void send_message_to_producer()
{
	print_debug(debug_print, "Sending message to producer thread started\n");
	while (shutdown_server == 0)
	{
		// get message from queue
		unique_lock<mutex> lock(message_queue_mutex);
		print_debug(debug_print, "Waiting for message to send to producer\n");
		message_queue_cv.wait(lock, [&]
							  { return !message_queue.empty() || shutdown_server == 1; });
		print_debug(debug_print, "Got message to send to producer\n");

		if (shutdown_server == 1)
		{
			break;
		}

		while (!message_queue.empty() && shutdown_server == 0)
		{
			// get message
			pair<string, string> to_send = message_queue.front();
			message_queue.pop();
			print_debug(debug_print, "Consumed [%s] from queue\n", to_send.second.c_str());
			// release lock
			message_queue_mutex.unlock();
			// send to producer
			producer->send_message_to_broker(to_send.first, to_send.second);
			message_queue_mutex.lock();
		}
	}
	print_debug(debug_print, "Sending message to producer thread closed\n");
}

void accept_frontend_connections()
{
	print_debug(debug_print, "Accepting frontend connections thread started\n");
	listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
	{
		cerr << "Cannot open socket" << endl;
		shutdown();
		return;
	}

	// ensure that after shutting down the server, you can bind to the same port right away
	int option = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(option));
	struct sockaddr_in serverAddr;
	bzero(&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(server_port);
	serverAddr.sin_addr.s_addr = inet_addr(server_ip.c_str());
	if (::bind(listen_fd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
	{
		cerr << "Failed to bind to frontend port" << strerror(errno) << endl;
		shutdown();
		return;
	}

	if (listen(listen_fd, FRONTEND_MAX_CONNECTIONS) < 0)
	{
		cerr << "Failed to listen for frontend connections" << endl;
		shutdown();
		return;
	}

	while (shutdown_server == 0)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(listen_fd, &r);
		int max_fd = listen_fd;

		fd_set w;
		FD_ZERO(&w);

		// add frontend connections
		for (int fd : frontend_connections)
		{
			// check if valid or closed
			int flag = fcntl(fd, F_GETFD);
			if (flag == -1)
			{
				closed_connections.push_back(fd);
			}
			else
			{
				FD_SET(fd, &r);
				FD_SET(fd, &w);
				max_fd = max(max_fd, fd);
			}
		}

		// remove closed connections
		for (int fd : closed_connections)
		{
			close(fd);
			string frontend_id = fd_frontendID[fd];
			fd_frontendID.erase(fd);
			frontendID_fd.erase(frontend_id);
			for (string user : frontendID_users[frontend_id])
			{
				if (user_frontendID.find(user) != user_frontendID.end())
				{
					consumer->remove_userID(user);
					cout << "Unsubscribing user from partition: " << user << endl;
				}
			}
			frontendID_users.erase(frontend_id);
		}
		closed_connections.clear();

		struct timeval timeout = {0, 0};
		int ret = select(max_fd + 1, &r, &w, NULL, &timeout);
		if (ret < 0)
		{
			cout << "Failed with select()" << endl;
			shutdown();
			return;
		}

		// if new client
		if (FD_ISSET(listen_fd, &r))
		{
			// accept the next incoming connection
			struct sockaddr_in src;
			socklen_t srclen = sizeof(src);
			int connfd = accept(listen_fd, (struct sockaddr *)&src, &srclen);
			// make non blocking
			// int flags = fcntl(connfd, F_GETFL, 0);
			// fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
			string client_ip_port = get_address(src);
			if (connfd < 0)
			{
				cerr << "Error with accept function in frontend thread" << endl;
				continue;
			}
			print_debug(debug_print, "New frontend connection FD: %d\n", connfd);
			print_debug(debug_print, "New frontend client IP: %s\n", client_ip_port.c_str());

			// send greeting message
			string greeting = "+OK Server ready\r\n";
			do_write(connfd, greeting.data(), greeting.size());
			frontend_connections.push_back(connfd);
		}

		// handle messages from users, put into message queue
		for (int fd : frontend_connections)
		{
			if (FD_ISSET(fd, &r))
			{
				cout << "reading inside the fd isset loop" << endl;
				string message = do_read(fd, "\n");
				if (message.find("USERNAME") != string::npos)
				{
					cout << "RECEIVED USERNAME" << endl;
					vector<string> parsed_message = split(message, ' ');
					if (parsed_message.size() != 3)
					{
						cerr << "Invalid USERNAME message format" << endl;
						continue;
					}
					string username = parsed_message.at(1);
					string frontend_server_id = parsed_message.at(2);
					user_frontendID[username] = frontend_server_id;
					frontendID_users[frontend_server_id].push_back(username);
					// subscribe to the user partition if not already subscribed
					consumer->add_userID(username);
					print_debug(debug_print, "Added user %s to consumer\n", username.c_str());
				}
				else if (message.find("DISCONNECTED") != string::npos)
				{
					print_debug(debug_print, "Received DISCONNECTED message from fd %d", fd);
					vector<string> disconnected_split = split(message, ' ');
					string dis_user = disconnected_split.at(1);
					string id = user_frontendID[dis_user];
					user_frontendID.erase(dis_user);
					consumer->remove_userID(dis_user);
				}
				else if (message.find("REGISTER") != string::npos)
				{
					// REGISTER serverID
					print_debug(debug_print, "Received REGISTER message from fd %d", fd);
					vector<string> register_split = split(message, ' ');
					frontendID_fd[register_split.at(1)] = fd;
					fd_frontendID[fd] = register_split.at(1);
				}
				else
				{
					// frontend sends SENDERID RECIPIENTID MESSAGE
					{
						// send message to producer
						// pair of recipient, message
						cout << "RECEIVED MESSAGE" << endl;
						vector<string> parsed_message = split(message, ' ');
						if (parsed_message.size() < 3)
						{
							cerr << "Invalid user message format" << endl;
							continue;
						}
						string recipient = parsed_message.at(1);
						pair<string, string> to_send = make_pair(recipient, message);
						lock_guard<mutex> lock(message_queue_mutex);
						message_queue.push(to_send);
						print_debug(debug_print, "Message added to message queue: %s\n", message.c_str());
					}
					message_queue_cv.notify_one();
				}
			}
		}
	}
	shutdown();
	print_debug(debug_print, "Accepting frontend connections thread closed\n");
}

// command args: ./backendserver -v IP:port serverID
int main(int argc, char **argv)
{
	signal(SIGINT, signal_handler);

	if (argc < 2)
	{
		cerr << "Incorrect usage" << endl;
		return EXIT_FAILURE;
	}

	int opt;
	while ((opt = getopt(argc, argv, "v")) != -1)
	{
		switch (opt)
		{
		case 'v':
			debug_print = true;
			break;
		default:
			cerr << "Incorrect flag usage" << endl;
			return EXIT_FAILURE;
		}
	}

	string IP_port = argv[optind];
	vector<string> IP_port_vector = split(IP_port, ':');
	server_ip = IP_port_vector.at(0);
	server_port = stoi(IP_port_vector.at(1));

	// get server ID for the kafka consumer group
	server_ID = argv[optind];

	// connect to Cassandra docker
	cassandra = new Cassandra("127.0.0.1");

	// create the Kafka producer and consumer
	producer = new KafkaProducer(brokers, "chat_messages");
	consumer = new KafkaConsumer(brokers, server_ID, "chat_messages");

	thread frontend_thread(accept_frontend_connections);			 // accept frontend connections
	thread sender_thread_frontend(send_message_to_frontend);		 // send message to frontend
	thread sender_thread_producer(send_message_to_producer);		 // send message to producer
	thread consumer_thread(&KafkaConsumer::poll_messages, consumer); // consumer poll messages from Kafka broker
	thread cassandra_thread(save_message_to_cassandra);

	// clean up resources
	frontend_thread.join();
	print_debug(debug_print, "Frontend thread joined\n");
	sender_thread_frontend.join();
	print_debug(debug_print, "Sender thread frontend joined\n");
	sender_thread_producer.join();
	print_debug(debug_print, "Sender thread producer joined\n");
	consumer_thread.join();
	print_debug(debug_print, "Polling thread joined\n");
	cassandra_thread.join();
	print_debug(debug_print, "Cassandra thread joined\n");

	delete producer;
	delete consumer;
	delete cassandra;

	return EXIT_SUCCESS;
}