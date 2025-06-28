#ifndef KAFKAPRODUCER_H
#define KAFKAPRODUCER_H

#include <string>
#include <librdkafka/rdkafka.h>

using namespace std;

class KafkaProducer
{
public:
	KafkaProducer(string broker, string topic);
	~KafkaProducer();
	int send_message_to_broker(string recipientID, string payload);

private:
	rd_kafka_t *producer;
	string topic;
};

#endif