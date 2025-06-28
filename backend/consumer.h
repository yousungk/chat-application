#ifndef KAFKACONSUMER_H
#define KAFKACONSUMER_H

#include <string>
#include <unordered_map>
#include <librdkafka/rdkafka.h>

using namespace std;

class KafkaConsumer
{
private:
	// TODO add mutex for this
	unordered_map<int, int> subscribed_partitions;
	rd_kafka_t *consumer;
	rd_kafka_topic_partition_list_t *subscription;
	std::string topic;
	std::string broker;

public:
	KafkaConsumer(string broker, string group, string topic);
	~KafkaConsumer();
	int add_userID(string userID);
	int remove_userID(string userID);
	void poll_messages();
};

#endif