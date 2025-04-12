#include <glib.h>
#include <librdkafka/rdkafka.h>
#include "common.cc"

using namespace std;

class KafkaProducer
{
public:
	KafkaProducer();
	~KafkaProducer();
	void set_config();
	void produce_message();
	void subscribe_to_partition(string user_id, int partition);
	void unsubscribe_to_partition(string user_id, int partition);

private:
	queue<string> message_queue;
	mutex message_queue_mutex;

}

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