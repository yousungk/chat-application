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
#include <glib.h>
#include <librdkafka/rdkafka.h>

using namespace std;

void set_config(rd_kafka_conf_t *conf, const char *key, const char *value)
{
	char errstr[512];
	if (rd_kafka_conf_set(conf, key, value, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
	{
		cerr << "Failed to set config " << key << ": " << errstr << endl;
		exit(1);
	}
}