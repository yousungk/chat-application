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

// TODO
// 1. Enable accepting browser clients via HTTP
// can handle multiple browser clients
// browser sends HTTP request to frontend server
// use Boost.Beast

// 2. Communicate with backend server using TCP
// POST /send
// GET /messages

// starting by ./chatclient backend server IP:port

using namespace std;

string IP;
string port;
int sock;

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
	cout << "Sent message: " << string(buf, len) << endl;
};

std::string do_read(int fd, std::string delim)
{
	std::string message;
	char buf[524288];
	while (true)
	{
		int r = read(fd, buf, sizeof(buf));
		// print_debug(debug_print, "Read from fd %d [%s]", fd, std::string(buf, r).c_str());
		if (r < 0)
		{
			std::cerr << "Error with do_read" << std::endl;
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
				std::string complete_message = message.substr(0, pos + 1);
				return complete_message;
			}
		}
	}
}

int main(int argc, char *argv[])
{

	string IP_port = argv[1];
	stringstream ss(IP_port);
	vector<string> IP_port_vector;
	string token;
	while (getline(ss, token, ':'))
	{
		IP_port_vector.push_back(token);
	}
	if (IP_port_vector.size() != 2)
	{
		cerr << "Invalid IP and port format" << endl;
		exit(1);
	}
	IP = IP_port_vector.at(0);
	port = IP_port_vector.at(1);

	// create socket to connect with backend
	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock < 0)
	{
		cerr << "Cannot open socket" << endl;
		exit(1);
	}

	// connect to the backend server
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(atoi(port.c_str()));
	inet_pton(AF_INET, IP.c_str(), &servaddr.sin_addr);

	if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
	{
		cerr << "Cannot connect to server" << endl;
		close(sock);
		exit(1);
	}

	// listen from standard input or from socket
	while (true)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(sock, &r);		  // add socket to read set
		FD_SET(STDIN_FILENO, &r); // add stdin to read set

		int ret = select(sock + 1, &r, NULL, NULL, NULL);
		if (ret < 0)
		{
			cerr << "Error with select function" << endl;
			exit(1);
		}

		// read from stdin and send message to server
		if (FD_ISSET(STDIN_FILENO, &r))
		{
			cout << "Read from stdin" << endl;
			// read from stdin
			string message = do_read(STDIN_FILENO, "\n");
			cout << "About to write to socket" << endl;
			// send message to server
			do_write(sock, message.c_str(), message.size());
		}

		// receive message from server and print to terminal
		if (FD_ISSET(sock, &r))
		{
			string message = do_read(sock, "\n");
			// print to terminal
			cout << message << endl;
		}
	}
	return 0;
}

void signal_handler(int signo)
{
	// set each connection to non-blocking (to avoid blocking on new system calls)
	if (signo == SIGINT)
	{
		// prepare destination address
		struct sockaddr_in dest;
		bzero(&dest, sizeof(dest));
		dest.sin_family = AF_INET;
		dest.sin_port = htons(atoi(port.c_str()));
		inet_pton(AF_INET, IP.c_str(), &(dest.sin_addr));

		// prepare message
		string message = "/quit";

		// send message to server
		int res = sendto(sock, message.c_str(), message.size(), 0, (struct sockaddr *)&dest, sizeof(dest));
		if (res < 0)
		{
			cerr << "Cannot send message to server" << endl;
			close(sock);
			exit(1);
		}

		// close the listening socket and terminate
		close(sock);
		exit(0);
	}
}