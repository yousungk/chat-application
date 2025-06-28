// SUMMARY:
// user <-> frontend: multithreaded for handling many browser connections

// frontend <-> backend: pool of TCP connections for handling frontend to backend requests
// requests for backend are stateless

// dedicated thread for receiving chat message from backend and sending to clients

// use redis to store user : frontend server mapping
// use mySQL for storing login info

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
#include <fcntl.h>
#include <mutex>
#include <memory>
#include <fstream>
#include <unordered_map>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include "tcp_pool.h"
#include "cassandra_client.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace std;
using WebSocketPtr = shared_ptr<websocket::stream<beast::tcp_stream>>;

volatile sig_atomic_t shutdown_server = 0;
string server_ip;	// backend server IP
string server_port; // backend server port
int backend_fd;		// file descriptor for the backend server connection
string frontend_server_ip;
string frontend_server_port;
string frontend_server_backend_port;
string server_id; // frontend server unique ID

unordered_map<string, string> users_cache;				  // cache for for username and password
unordered_map<string, vector<string>> user_friends_cache; // cache for user friends

int TCP_MAX_CONNECTIONS = 200;					// number of max TCP connections to backend
const int USER_MAX_CONNECTIONS = 200;			// number of max threads for accepting clients
pthread_t threads[USER_MAX_CONNECTIONS];		// array of thread pid
int fds[USER_MAX_CONNECTIONS] = {-1};			// array of file descriptors served by threads
unordered_map<string, WebSocketPtr> user_to_wb; // username : web socket pointer
mutex user_to_wb_mutex;
TCPConnectionPool *tcp_pool; // TCP backend connection pool

Cassandra *cassandra;						   // connection to Cassandra db
unordered_map<string, int64_t> chathistory_ts; // username  : timestamp

void *connect_to_backend(void *);
void *handle_http_client(void *);

// HELPER FUNCTIONS /////////////////////////////////////////////////////////////////////////////
void shutdown()
{
	shutdown_server = 1;
}

void signal_handler(int signo)
{
	cout << "Received terminal signal" << endl;
	if (signo == SIGINT)
	{
		shutdown_server = 1;
	}
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

int find_available_fd()
{
	for (int i = 0; i < USER_MAX_CONNECTIONS; i++)
	{
		if (fds[i] == -1)
		{
			return i;
		}
	}
	return -1;
}

string read_file(const string &path)
{
	ifstream f(path);
	if (!f)
	{
		return "";
	}
	return string((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
}

string get_content_type(const string &path)
{
	if (path.ends_with(".html"))
		return "text/html";
	if (path.ends_with(".css"))
		return "text/css";
	if (path.ends_with(".js"))
		return "application/javascript";
	return "application/octet-stream";
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

string do_read(int fd, string delim)
{
	string message;
	char buf[524288];
	while (true)
	{
		int r = read(fd, buf, sizeof(buf));
		if (r < 0)
		{
			cerr << "Error with do_read" << strerror(errno) << endl;
			return "ERROR";
		}
		else if (r == 0)
		{
			cerr << "Connection closed during do_read" << endl;
			close(fd);
			return "CLOSED";
		}
		else
		{
			message.append(buf, r);
			size_t pos = message.find(delim);
			if (pos != string::npos)
			{
				string complete_message = message.substr(0, pos);
				return complete_message;
			}
		}
	}
}

// THREAD FUNCTIONS ////////////////////////////////////////////////////////////////////////////////////
// thread to receive message from backend, and send to frontend websocket
void *connect_to_backend(void *arg)
{
	backend_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (backend_fd < 0)
	{
		perror("Socket creation failed");
		exit(EXIT_FAILURE);
	}

	// Bind to a specific local port
	struct sockaddr_in serverAddr;
	bzero(&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(atoi(frontend_server_backend_port.c_str()));
	serverAddr.sin_addr.s_addr = inet_addr(frontend_server_ip.c_str());

	cout << "Using PORT for connecting to backend: " << frontend_server_backend_port << endl;
	if (::bind(backend_fd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
	{
		cerr << "Failed to bind to port" << strerror(errno) << endl;
		shutdown();
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in dest;
	bzero(&dest, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(atoi(server_port.c_str()));
	inet_pton(AF_INET, server_ip.c_str(), &(dest.sin_addr));

	if (connect(backend_fd, (struct sockaddr *)&dest, sizeof(dest)) < 0)
	{
		perror("Connection to server failed");
		close(backend_fd);
		exit(EXIT_FAILURE);
	}

	// send frontend ID to backend
	string register_msg = "REGISTER " + server_id + "\n";
	do_write(backend_fd, register_msg.c_str(), register_msg.size());

	while (shutdown_server == 0 && fcntl(backend_fd, F_GETFD) != -1)
	{
		cout << "Backend thread listening for messages" << endl;
		// SENDER RECIPIENT MESSAGE
		string message = do_read(backend_fd, "\n");
		cout << "Read message: " + message << endl;
		vector<string> split_message = split(message, ' ');
		if (split_message.size() < 3)
		{
			continue;
		}
		string recipient = split_message.at(1);
		if (message.find("+OK Server ready") != string::npos)
		{
			cout << "Received OKAY from backend server" << endl;
		}
		else if (user_to_wb.find(recipient) != user_to_wb.end())
		{
			user_to_wb[recipient]->text(true);
			user_to_wb[recipient]->write(net::buffer(message));
			cout << "Chat message sent to user " << recipient << " FD " << user_to_wb[recipient] << ": " << message << endl;
		}
		else
		{
			cout << "Recipient not connected: " << recipient << endl;
		}
	}

	close(backend_fd);
	pthread_exit(NULL);
}

// get HTTP requests
void *handle_http_client(void *arg)
{
	cout << "Handling client inside http client function" << endl;
	int current_fd_idx = *(int *)arg;
	int client_fd = fds[current_fd_idx];
	string username;

	net::io_context ioc;
	beast::tcp_stream stream(ioc);
	stream.socket().assign(tcp::v4(), client_fd);

	while (fcntl(client_fd, F_GETFD) != -1 && shutdown_server == 0)
	{
		try
		{
			cout << "About to read request" << endl;
			beast::flat_buffer buffer;
			http::request<http::string_body> req;
			http::read(stream, buffer, req);

			// Convert the buffer's content to a string
			string request_data{
				static_cast<const char *>(buffer.data().data()),
				buffer.size()};

			cout << req << endl;

			// Get username from cookie
			if (req.find(http::field::cookie) != req.end())
			{
				string cookie_header = string(req[http::field::cookie]);
				vector<string> cookies = split(cookie_header, ';');
				for (const auto &cookie : cookies)
				{
					vector<string> cookie_words = split(cookie, '=');
					if (cookie_words.at(0) == "username")
					{
						username = cookie_words.at(1);
						break;
					}
				}
			}

			// the request is a WebSocket upgrade
			if (websocket::is_upgrade(req))
			{
				auto ws = make_shared<websocket::stream<beast::tcp_stream>>(move(stream));
				user_to_wb[username] = ws;
				ws->accept(req);
				int fd = tcp_pool->acquire_connection();
				// send USERNAME username SERVERID
				cout << "Sending username to backend" << endl;
				string username_msg = "USERNAME " + username + " " + server_id + "\n";
				do_write(fd, username_msg.c_str(), username_msg.size());
				while (ws->is_open() && shutdown_server == 0)
				{
					cout << "Waiting to receive WS message for user:" << username << endl;
					beast::flat_buffer buffer;
					try
					{
						ws->read(buffer);
						string msg = beast::buffers_to_string(buffer.data());
						msg += "\n";
						cout << "Received chat message: " << msg << endl;
						// find the user fd and send to recipient
						do_write(fd, msg.c_str(), msg.size());
					}
					catch (const boost::system::system_error &e)
					{
						// Specific error handling for WebSocket read operations
						if (e.code() == beast::websocket::error::closed ||
							e.code() == beast::http::error::end_of_stream ||
							e.code() == net::error::eof)
						{
							cout << "WebSocket connection for user " << username << " gracefully closed." << endl;
						}
						else
						{
							cerr << "Error reading from WebSocket for user " << username << ": " << e.what() << endl;
						}
						break;
					}
				}
				cout << "Client disconnected" << endl;
				// send disconnect message to backend
				string dis_msg = "DISCONNECTED " + username + "\n";
				do_write(fd, dis_msg.c_str(), dis_msg.size());
				tcp_pool->release_connection(fd);
				{
					lock_guard<mutex> lock(user_to_wb_mutex);
					user_to_wb[username]->close(boost::beast::websocket::close_code::normal);
					user_to_wb.erase(username);
				}
				close(client_fd);
				fds[current_fd_idx] = -1;
				pthread_exit(NULL);
			}
			else
			{
				// HTTP ENDPOINTS
				string target = string(req.target());
				auto method = req.method();
				http::response<http::string_body> res;
				// HTML/CSS requests
				if (method == http::verb::get && target == "/")
				{
					cout << "Serving main page" << endl;
					target = "frontend/templates/chat.html";
					string body = read_file(target);
					res.result(http::status::ok);
					res.set(http::field::content_type, get_content_type(target));
					res.body() = body;
				}
				else if (method == http::verb::get && target == "/css/style.css")
				{
					target = "frontend/css/style.css";
					string body = read_file(target);
					res.result(http::status::ok);
					res.set(http::field::content_type, get_content_type(target));
					res.body() = body;
				}
				// POST LOGIN
				else if (method == http::verb::post && target == "/login")
				{
					cout << "Received LOGIN" << endl;
					string body = req.body();
					boost::json::value json = boost::json::parse(body);
					string username = boost::json::value_to<string>(json.at("providedUsername"));
					string password = boost::json::value_to<string>(json.at("providedPassword"));
					res.set(http::field::content_type, "text/plain");
					// first check cache
					if (users_cache.find(username) != users_cache.end() && users_cache[username] == password)
					{
						cout << "LOGIN: user inside cache" << endl;
						res.result(http::status::ok);
						res.body() = "Login successful";
					}
					// otherwise check db
					else
					{
						cout << "LOGIN: checking database" << endl;
						int result = cassandra->get_user_from_db(username, password);
						if (result == 1)
						{
							res.result(http::status::ok);
							res.body() = "Login successful";
							// put into cache
							users_cache[username] = password;
						}
						else
						{
							res.result(http::status::unauthorized);
							res.body() = "Invalid username or password";
						}
					}
				}
				// POST SIGNUP
				else if (method == http::verb::post && target == "/signup")
				{
					// check if user already exists in database
					string body = req.body();
					boost::json::value json = boost::json::parse(body);
					string username = boost::json::value_to<string>(json.at("newUsername"));
					string password = boost::json::value_to<string>(json.at("newPassword"));
					res.set(http::field::content_type, "text/plain");
					if (users_cache.find(username) != users_cache.end())
					{
						res.result(http::status::conflict);
						res.body() = "User already exists";
					}
					else
					{
						// Add the user to the database
						int result = cassandra->save_user_to_db(username, password);
						if (result == -1)
						{
							res.result(http::status::conflict);
							res.body() = "Signup failed";
						}
						else
						{
							// Add use to cache
							users_cache[username] = password;
							res.result(http::status::ok);
							res.body() = "Signup successful";
						}
					}
				}
				// GET messages/username/friend
				// request: CHATHISTORY USERNAME FRIEND OFFSET
				// response: SENDER RECIPIENT MESSAGE\n
				else if (method == http::verb::get && target.starts_with("/messages/"))
				{
					vector<string> split_target = split(target, '/');
					string username = split_target.at(2);
					string wanted_friend = split_target.at(3);
					int64_t ts;
					if (chathistory_ts.find(username) == chathistory_ts.end())
					{
						cout << "Using current timestamp for chat history" << endl;
						auto current_time = chrono::system_clock::now();
						ts = chrono::duration_cast<chrono::milliseconds>(
								 current_time.time_since_epoch())
								 .count();
					}
					else
					{
						cout << "Using stored timestamp for chat history" << endl;
						ts = chathistory_ts[username];
					}
					string history = cassandra->get_chat_history_from_db(username, wanted_friend, ts);
					size_t last_newline = history.rfind('\n');
					if (last_newline != string::npos)
					{
						// Extract the timestamp (everything after the last newline)
						string timestamp = history.substr(last_newline + 1);

						// Print the extracted timestamp
						cout << "Timestamp: " << timestamp << endl;
						if (timestamp != "")
						{
							int64_t timestamp_int = stoll(timestamp);
							chathistory_ts[username] = timestamp_int;
						}
					}
					else
					{
						cerr << "No history found from Cassandra" << endl;
					}
					history = history.substr(0, last_newline + 1);
					res.result(http::status::ok);
					res.set(http::field::content_type, "text/plain");
					res.body() = history;
				}
				// GET friends/username
				// request: FRIEND USERNAME
				// response FRIEND\n
				// get the list of friends for the username
				else if (method == http::verb::get && target.starts_with("/friends/"))
				{
					vector<string> split_target = split(target, '/');
					string requested_user = split_target.at(2);
					// retrieve friend list from cache
					vector<string> friend_list;
					if (user_friends_cache.find(requested_user) != user_friends_cache.end())
					{
						friend_list = user_friends_cache[requested_user];
					}
					// otherwise get friend list from db
					else
					{
						friend_list = cassandra->get_friend_list_from_db(requested_user);
						// put friend list into cache
						if (friend_list.size() > 0)
						{
							user_friends_cache[requested_user] = friend_list;
						}
					}
					ostringstream oss;
					if (friend_list.size() > 0)
					{
						for (const auto &str : friend_list)
						{
							oss << str << "\n";
						}
						res.body() = oss.str();
					}
					cout << "Got friend from username " << requested_user << ": " << res.body() << endl;
					res.result(http::status::ok);
					res.set(http::field::content_type, "text/plain");
				}
				// POST method to add new friend
				else if (method == http::verb::post && target.starts_with("/friends"))
				{
					string body = req.body();
					boost::json::value json = boost::json::parse(body);
					string new_friend = boost::json::value_to<string>(json.at("friendName"));
					string user = boost::json::value_to<string>(json.at("username"));
					res.set(http::field::content_type, "text/plain");
					// add to database
					cassandra->save_friend_to_db(user, new_friend);
					// add to friend cache
					user_friends_cache[user]
						.push_back(new_friend);
					res.body() = "Friend sucessfully added";
					res.result(http::status::ok);
				}
				else if (method == http::verb::get && target == "/.well-known/appspecific/com.chrome.devtools.json")
				{
					res.set(http::field::content_type, "application/json");
					res.body() = "{}";
				}
				else
				{
					cout << "Unknown route" << endl;
					res.result(http::status::not_found);
					res.set(http::field::content_type, "text/plain");
					res.body() = "404 - Not Found";
				}
				res.version(req.version());
				res.prepare_payload();
				// cout << res << endl;
				http::write(stream, res);
			}
		}
		catch (const boost::system::system_error &e)
		{
			if (e.code() == beast::http::error::end_of_stream)
			{
				cout << "Reached end of stream" << endl;
			}
			else
			{
				cerr << "Error: " << e.what() << endl;
			}
			break;
			// stream.socket().shutdown(tcp::socket::shutdown_send);
		}
	}
	// connection closed
	cout << "Client disconnected" << endl;
	if (chathistory_ts.find(username) != chathistory_ts.end())
	{
		chathistory_ts.erase(username);
	}
	if (fcntl(client_fd, F_GETFD) != -1)
	{
		tcp_pool->release_connection(client_fd);
	}
	delete static_cast<int *>(arg);
	fds[current_fd_idx] = -1;
	pthread_exit(NULL);
	return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////
// command args: ./frontend_server_real backend IP:port frontend IP:accept client port:connect to backend port frontend-server-ID
int main(int argc, char **argv)
{
	signal(SIGINT, signal_handler);

	if (argc < 4)
	{
		cerr << "Incorrect usage" << endl;
		return EXIT_FAILURE;
	}

	string ip_port = argv[1];
	vector<string> ip_port_vector = split(ip_port, ':');
	server_ip = ip_port_vector.at(0);
	server_port = ip_port_vector.at(1);

	string frontend_ip_port = argv[2];
	vector<string> frontend_ip_port_vector = split(frontend_ip_port, ':');
	frontend_server_ip = frontend_ip_port_vector.at(0);
	frontend_server_port = frontend_ip_port_vector.at(1);
	frontend_server_backend_port = frontend_ip_port_vector.at(2);

	server_id = argv[3];

	// backend can send chat message here
	pthread_t backend_thread;
	pthread_create(&backend_thread, nullptr, connect_to_backend, nullptr);

	// pool of TCPs for handling backend communication
	tcp_pool = new TCPConnectionPool(server_ip, stoi(server_port), TCP_MAX_CONNECTIONS);

	// connect to cassandra for user, friend, and chat history data
	cassandra = new Cassandra("127.0.0.1");

	// start accepting users
	int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
	{
		cerr << "Cannot open socket" << endl;
		shutdown();
		return EXIT_FAILURE;
	}

	int option = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(option));
	struct sockaddr_in serverAddr;
	bzero(&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(atoi(frontend_server_port.c_str()));
	serverAddr.sin_addr.s_addr = inet_addr(frontend_server_ip.c_str());

	cout << "Using PORT for accepting HTTP: " << frontend_server_port << endl;
	if (::bind(listen_fd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
	{
		cerr << "Failed to bind to port" << strerror(errno) << endl;
		shutdown();
		return EXIT_FAILURE;
	}

	if (listen(listen_fd, USER_MAX_CONNECTIONS) < 0)
	{
		cerr << "Failed to listen for frontend connections" << endl;
		shutdown();
		return EXIT_FAILURE;
	}

	cout << "Started listening for clients" << endl;
	while (shutdown_server == 0)
	{
		// accept new user
		int available_fd = find_available_fd();
		struct sockaddr_in src;
		socklen_t srclen = sizeof(src);
		fds[available_fd] = accept(listen_fd, (struct sockaddr *)&src, &srclen);
		cout << "Serving client in fd: " << fds[available_fd] << endl;

		// create new thread to handle user connection
		int *arg = new int(available_fd);
		pthread_create(&(threads[available_fd]), nullptr, handle_http_client, (void *)arg);
	}

	// close all user connections
	pthread_join(backend_thread, nullptr);
	cout << "Joined backend thread" << endl;
	close(listen_fd);
	for (int i = 0; i < USER_MAX_CONNECTIONS; i++)
	{
		if (fds[i] != -1)
		{
			close(fds[i]);
			pthread_join(threads[i], nullptr);
			cout << "Joined user thread" << endl;
		}
	}
	{
		lock_guard<mutex> lock(user_to_wb_mutex);
		for (auto user_pair : user_to_wb)
		{
			user_pair.second->close(boost::beast::websocket::close_code::normal);
		}
	}

	delete cassandra;
	return EXIT_SUCCESS;
}