#include "tcp_pool.h"

int TCPConnectionPool::create_connection()
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
	{
		perror("Socket creation failed");
		return -1;
	}

	sockaddr_in backend_addr{};
	backend_addr.sin_family = AF_INET;
	backend_addr.sin_port = htons(backend_port);
	inet_pton(AF_INET, backend_ip.c_str(), &backend_addr.sin_addr);

	if (connect(sock, (struct sockaddr *)&backend_addr, sizeof(backend_addr)) < 0)
	{
		perror("Connection to backend failed");
		close(sock);
		return -1;
	}
	return sock;
}

TCPConnectionPool::TCPConnectionPool(const std::string &ip, int port, int size)
	: backend_ip(ip), backend_port(port), pool_size(size)
{
	// Initialize the pool with connections
	for (int i = 0; i < pool_size; ++i)
	{
		int conn = create_connection();
		if (conn >= 0)
		{
			pool.push(conn);
		}
	}
}

TCPConnectionPool::~TCPConnectionPool()
{
	// Close all connections in the pool
	while (!pool.empty())
	{
		close(pool.front());
		pool.pop();
	}
}

// Acquire a connection from the pool
int TCPConnectionPool::acquire_connection()
{
	std::unique_lock<std::mutex> lock(pool_mutex);
	pool_cv.wait(lock, [this]()
				 { return !pool.empty(); });

	int conn = pool.front();
	pool.pop();
	return conn;
}

// Release a connection back to the pool
void TCPConnectionPool::release_connection(int conn)
{
	std::unique_lock<std::mutex> lock(pool_mutex);
	pool.push(conn);
	lock.unlock();
	pool_cv.notify_one();
}