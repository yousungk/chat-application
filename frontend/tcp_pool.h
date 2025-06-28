#ifndef TCP_POOL_H
#define TCP_POOL_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>

class TCPConnectionPool
{
private:
	std::queue<int> pool;
	std::mutex pool_mutex;
	std::condition_variable pool_cv;
	std::string backend_ip;
	int backend_port;
	int pool_size;
	int create_connection();

public:
	TCPConnectionPool(const std::string &ip, int port, int size);
	~TCPConnectionPool();
	int acquire_connection();
	void release_connection(int conn);
};

#endif