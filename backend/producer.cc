#include "producer.h"
#include "externs.h"
#include "utils.h"
#include <glib.h>
#include <librdkafka/rdkafka.h>

using namespace std;

// callback function for if message is delivered or fails
// triggered by poll() or flush()
static void
dr_msg_cb(rd_kafka_t *kafka_handle,
		  const rd_kafka_message_t *rkmessage,
		  void *opaque)
{
	if (rkmessage->err)
	{
		print_debug(debug_print, "Message delivery failed: %s\n", rd_kafka_err2str(rkmessage->err));
	}
	else
	{
		print_debug(debug_print, "Message delivered to topic %s partition [%d] at offset %ld\n",
					rd_kafka_topic_name(rkmessage->rkt),
					rkmessage->partition,
					rkmessage->offset);
	}
}

// need to connect to only one broker in the cluster
// then the broker will provide with metadata of address of all other brokers, including the topic and partitions they manage
// this way can dynamically connect to correct broker
KafkaProducer::KafkaProducer(string broker, string assigned_topic)
{
	this->topic = assigned_topic;
	rd_kafka_conf_t *conf = rd_kafka_conf_new();
	char errstr[512];

	if (rd_kafka_conf_set(
			conf,
			"bootstrap.servers",
			broker.c_str(),
			errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		throw runtime_error(string("Failed to set bootstrap.servers: ") + errstr);
	}

	if (rd_kafka_conf_set(
			conf,
			"acks",
			"all",
			errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		throw runtime_error(string("Failed to set acks: ") + errstr);
	}

	if (rd_kafka_conf_set(conf,
						  "security.protocol",
						  "PLAINTEXT", // or "SASL_SSL"
						  errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		throw runtime_error(string("Failed to set security.protocol: ") + errstr);
	}

	// install a delivery-error callback
	rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);

	producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
	if (!producer)
	{
		cerr << "Failed to create producer: " << errstr << endl;
	}

	// configuration object is now owned, and freed, by the rd_kafka_t instance
	conf = NULL;

	rd_kafka_poll(producer, 1000);
}

KafkaProducer::~KafkaProducer()
{
	rd_kafka_flush(producer, 10 * 1000);
	rd_kafka_destroy(producer);
}

int KafkaProducer::send_message_to_broker(string recipientID, string payload)
{
	int partition = get_partition(recipientID);
	print_debug(debug_print, "Sending message to broker with topic %s partition %d\n", topic.c_str(), partition);

	rd_kafka_resp_err_t err = rd_kafka_producev(producer,
												RD_KAFKA_V_TOPIC(topic.c_str()),
												RD_KAFKA_V_PARTITION(partition),
												RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
												RD_KAFKA_V_VALUE((void *)payload.c_str(), payload.size()),
												RD_KAFKA_V_OPAQUE(NULL),
												RD_KAFKA_V_END);
	if (err)
	{
		print_debug(debug_print, "Failed to send message to producer: %s\n", rd_kafka_err2str(err));
		return -1;
	}
	rd_kafka_poll(producer, 1000);
	return 1;
}
