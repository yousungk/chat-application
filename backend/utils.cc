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
#include "utils.h"
#include "externs.h"

using namespace std;

int get_partition(string key)
{
	return key.at(0) % PARTITION_COUNT;
}

vector<string> split(string message, char delimiter)
{
	stringstream ss(message);
	string segment;
	vector<string> response;
	while (getline(ss, segment, delimiter))
	{
		response.push_back(segment);
	}
	return response;
}

std::string get_address(struct sockaddr_in &addr)
{
	char sender_ip[16];
	inet_ntop(AF_INET, &addr.sin_addr, sender_ip, sizeof(sender_ip));
	auto port = ntohs(addr.sin_port);
	std::string address = std::string(sender_ip) + ":" + std::to_string(port);
	return address;
}

void do_write(int fd, const char *buf, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = write(fd, &buf[sent], len - sent);
		// if write fails, then close connection and close thread
		if (n < 0)
		{
			perror("ERROR: Write failed");
			close(fd);
			pthread_exit(NULL);
		}
		sent += n;
	}
};

std::string do_read(int fd, std::string delim)
{
	std::string message;
	char buf[524288];
	while (true)
	{
		int r = read(fd, buf, sizeof(buf));
		print_debug(debug_print, "Read from fd %d [%s]\n", fd, std::string(buf, r).c_str());
		if (r < 0)
		{
			std::cerr << "Error with do_read" << strerror(errno) << std::endl;
			return "ERROR";
		}
		else if (r == 0)
		{
			std::cerr << "Connection closed during do_read" << std::endl;
			close(fd);
			return "CLOSED";
		}
		else
		{
			message.append(buf, r);
			size_t pos = message.find(delim);
			if (pos != std::string::npos)
			{
				std::string complete_message = message.substr(0, pos);
				return complete_message;
			}
		}
	}
}

void print_debug(bool debug_print, const char *message, ...)
{
	if (debug_print == true)
	{
		va_list args;
		va_start(args, message);
		vfprintf(stderr, message, args);
		va_end(args);
		// Immediately flush stderr to ensure the message prints right away.
		fflush(stderr);
	}
}