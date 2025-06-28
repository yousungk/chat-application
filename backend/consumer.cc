#include "consumer.h"
#include "utils.h"
#include "externs.h"

using namespace std;

KafkaConsumer::KafkaConsumer(string broker, string group, string topic)
{
	this->topic = topic;
	this->broker = broker;

	rd_kafka_conf_t *conf = rd_kafka_conf_new();
	char errstr[512];

	// Set "bootstrap.servers"
	if (rd_kafka_conf_set(conf, "bootstrap.servers", broker.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		std::cerr << "Error setting bootstrap.servers: " << errstr << std::endl;
	}

	// Set "group.id"
	if (rd_kafka_conf_set(conf, "group.id", group.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		std::cerr << "Error setting group.id: " << errstr << std::endl;
	}

	// Set "auto.offset.reset"
	if (rd_kafka_conf_set(conf, "auto.offset.reset", "earliest", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		std::cerr << "Error setting auto.offset.reset: " << errstr << std::endl;
	}

	// Create the Consumer instance
	consumer = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
	if (!consumer)
	{
		throw std::runtime_error("Error while creating consumer instance");
	}
	rd_kafka_poll_set_consumer(consumer);

	// Configuration object is now owned, and freed, by the rd_kafka_t instance
	conf = NULL;

	// Create list of subscription
	subscription = rd_kafka_topic_partition_list_new(0);
	rd_kafka_assign(consumer, subscription);
}

KafkaConsumer::~KafkaConsumer()
{
	rd_kafka_topic_partition_list_destroy(subscription);
	rd_kafka_consumer_close(consumer);
	rd_kafka_destroy(consumer);
}

int KafkaConsumer::add_userID(string userID)
{
	int partition = get_partition(userID);

	print_debug(debug_print, "Adding user %s to partition %d\n", userID.c_str(), partition);
	if (subscribed_partitions.find(partition) == subscribed_partitions.end())
	{
		// subscribe
		rd_kafka_topic_partition_list_add(subscription, this->topic.c_str(), partition);
		rd_kafka_resp_err_t err = rd_kafka_assign(consumer, subscription);
		if (err)
		{
			cerr << "Failed to subscribe to partition " << partition << endl;
			return -1;
		}

		subscribed_partitions[partition] = 1;
		return 1;
	}
	subscribed_partitions[partition] += 1;
	return 1;
}

int KafkaConsumer::remove_userID(string userID)
{
	int partition = get_partition(userID);
	subscribed_partitions[partition] -= 1;

	if (subscribed_partitions[partition] == 0)
	{
		subscribed_partitions.erase(partition);
		rd_kafka_topic_partition_list_del(subscription, this->topic.c_str(), partition);
		rd_kafka_resp_err_t err = rd_kafka_assign(consumer, subscription);
		if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
		{
			std::cerr << "Failed to reassign after removing partition: " << rd_kafka_err2str(err) << std::endl;
			return -1;
		}
	}
	return 1;
}

// function used to send message to frontend users
void KafkaConsumer::poll_messages()
{
	while (shutdown_server == 0)
	{
		rd_kafka_message_t *consumer_message = rd_kafka_consumer_poll(consumer, 500);

		if (!consumer_message)
		{
			cout << ("Waiting...") << endl;
			continue;
		}
		if (consumer_message->err)
		{
			if (consumer_message->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
			{
				/* We can ignore this error - it just means we've read
				 * everything and are waiting for more data.
				 */
			}
			else
			{
				cerr << (rd_kafka_message_errstr(consumer_message)) << endl;
			}
		}
		else
		{
			print_debug(debug_print, "Received message from Kafka broker");
			// RECIPIENT SENDER MESSAGE
			string message(static_cast<char *>(consumer_message->payload), consumer_message->len);
			{
				lock_guard<mutex> lock(message_lock);
				print_debug(debug_print, "Adding message to send to frontend: %s", message.c_str());
				message_to_send.push(message);
				message_to_save.push(message);
			}
		}

		// Free the message when we're done
		rd_kafka_message_destroy(consumer_message);
	}
}