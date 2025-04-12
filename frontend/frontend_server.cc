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

// functions to support
// MESSAGE
// /send user message number
// /send group message number
// 1 user message
// 2 group message
// starting by ./chatclient IP:port

using namespace std;

string IP;
string port;
int sock;

int main(int argc, char *argv[])
{

	string IP_port = argv[1];
	stringstream ss(IP_port);
	vector<string> IP_port_vector : string token;
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
			// read from stdin
			char buf[1000];
			int n = read(STDIN_FILENO, buf, 1000);
			if (n < 0)
			{
				cerr << "Cannot read from stdin" << endl;
				close(sock);
				exit(1);
			}

			// remove trailing <CR>, <LF>, or <CRLF>
			if (n >= 1 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			{
				if (n > 1 && buf[n - 1] == '\n' && buf[n - 2] == '\r')
				{
					buf[n - 2] = '\0'; // remove <CRLF>
					n -= 2;
				}
				else
				{
					buf[n - 1] = '\0'; // remove <CR> or <LF>
					n -= 1;
				}
			}

			// send message to server
			do_write(sock, buf, n);

			// if quit, terminate
			string string_buf = string(buf);
			if (string_buf == "/quit")
			{
				close(sock);
				exit(0);
			}
		}

		// receive message from server and print to terminal
		if (FD_ISSET(sock, &r))
		{
			char buf[1000];
			int rlen = do_read(sock, buf, sizeof(buf) - 1);

			// null terminate
			buf[rlen] = '\0';

			// print to terminal
			cout << buf << endl;
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

int do_read(int fd, char *buf, int len)
{
	int received = 0;
	while (true)
	{
		int n = read(fd, &buf[received], len - received);
		// if read fails, then close connection and close thread
		if (n < 0)
		{
			perror("ERROR: Read failed");
			close(fd);
			pthread_exit(NULL);
		}
		received += n;
	}
	return received;
};