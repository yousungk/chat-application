#include <cassandra.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sstream>
#include <stdio.h>
#include "cassandra_client.h"

using namespace std;

Cassandra::Cassandra(string address)
{
	/* Setup and connect to cluster */
	connect_future = NULL;
	cluster = cass_cluster_new();
	session = cass_session_new();

	/* Add contact points */
	cass_cluster_set_contact_points(cluster, address.c_str());

	/* Provide the cluster object as configuration to connect the session */
	connect_future = cass_session_connect(session, cluster);

	/* This operation will block until the result is ready */
	CassError rc = cass_future_error_code(connect_future);

	if (rc != CASS_OK)
	{
		/* Display connection error message */
		const char *message;
		size_t message_length;
		cass_future_error_message(connect_future, &message, &message_length);
		fprintf(stderr, "Connect error: '%.*s'\n", (int)message_length, message);
	}

	// set keyspace
	CassStatement *use_keyspace_statement = cass_statement_new("USE chatapp", 0);
	CassFuture *use_keyspace_future = cass_session_execute(session, use_keyspace_statement);
	cass_statement_free(use_keyspace_statement);

	if (cass_future_error_code(use_keyspace_future) != CASS_OK)
	{
		const char *error_message;
		size_t error_message_length;
		cass_future_error_message(use_keyspace_future, &error_message, &error_message_length);
		fprintf(stderr, "Error setting keyspace: %.*s\n", (int)error_message_length, error_message);
		cass_future_free(use_keyspace_future);
	}
	cass_future_free(use_keyspace_future);
}

Cassandra::~Cassandra()
{
	cass_future_free(connect_future);
	cass_session_free(session);
	cass_cluster_free(cluster);
}

int Cassandra::save_message_to_db(const string &user1, const string &user2, const string &sender, const string &message)
{
	// Prepare the query
	const char *query = "INSERT INTO conversation (user1, user2, timestamp, sender, message) VALUES (?, ?, ?, ?, ?);";
	CassStatement *statement = cass_statement_new(query, 5);

	// calculate timestamp
	auto now = chrono::system_clock::now();
	int64_t ts = chrono::duration_cast<chrono::milliseconds>(
					 now.time_since_epoch())
					 .count();

	// Bind the values
	cass_statement_bind_string(statement, 0, user1.c_str());   // user1
	cass_statement_bind_string(statement, 1, user2.c_str());   // user2
	cass_statement_bind_int64(statement, 2, ts);			   // timestamp
	cass_statement_bind_string(statement, 3, sender.c_str());  // sender
	cass_statement_bind_string(statement, 4, message.c_str()); // message

	// Execute the query
	CassFuture *query_future = cass_session_execute(session, statement);
	cass_statement_free(statement);

	/* This will block until the query has finished */
	CassError rc = cass_future_error_code(query_future);

	if (rc != CASS_OK)
	{
		const char *error_message;
		size_t error_message_length;
		cass_future_error_message(query_future, &error_message, &error_message_length);
		fprintf(stderr, "Error: %.*s\n", (int)error_message_length, error_message);
		cass_future_free(query_future);
		return -1;
	}

	cass_future_free(query_future);
	return 1;
}

int Cassandra::save_user_to_db(const string &username, const string &password)
{
	cout << "Saving user to Cassandra" << endl;
	const char *query = "INSERT INTO users (username, password) VALUES (?, ?);";
	CassStatement *statement = cass_statement_new(query, 2);

	cass_statement_bind_string(statement, 0, username.c_str());
	cass_statement_bind_string(statement, 1, password.c_str());

	CassFuture *query_future = cass_session_execute(session, statement);
	cass_statement_free(statement);

	/* This will block until the query has finished */
	CassError rc = cass_future_error_code(query_future);

	if (rc != CASS_OK)
	{
		const char *error_message;
		size_t error_message_length;
		cass_future_error_message(query_future, &error_message, &error_message_length);
		fprintf(stderr, "Error: %.*s\n", (int)error_message_length, error_message);
		cass_future_free(query_future);
		return -1;
	}

	cass_future_free(query_future);
	return 1;
}

int Cassandra::save_friend_to_db(const string &username, const string &_friend)
{
	const char *query = "INSERT INTO friends (user1, user2) VALUES (?, ?);";
	CassStatement *statement = cass_statement_new(query, 2);

	cass_statement_bind_string(statement, 0, username.c_str());
	cass_statement_bind_string(statement, 1, _friend.c_str());

	CassFuture *query_future = cass_session_execute(session, statement);
	cass_statement_free(statement);

	/* This will block until the query has finished */
	CassError rc = cass_future_error_code(query_future);

	if (rc != CASS_OK)
	{
		const char *error_message;
		size_t error_message_length;
		cass_future_error_message(query_future, &error_message, &error_message_length);
		fprintf(stderr, "Error: %.*s\n", (int)error_message_length, error_message);
		cass_future_free(query_future);
		return -1;
	}

	cass_future_free(query_future);
	return 1;
}

vector<string> Cassandra::get_friend_list_from_db(const string &username)
{
	cout << "GET FRIENDS: getting friends from database for user " << username << endl;
	vector<string> friend_list;

	// Prepare the query
	const char *query = "SELECT user2 FROM friends WHERE user1 = ?;";
	CassStatement *statement = cass_statement_new(query, 1);

	// Bind the username to the query
	cass_statement_bind_string(statement, 0, username.c_str());

	// Execute the query
	CassFuture *query_future = cass_session_execute(session, statement);
	cass_statement_free(statement);

	// Handle the query result
	const CassResult *result = cass_future_get_result(query_future);
	if (result == NULL)
	{
		CassError rc = cass_future_error_code(query_future);
		printf("Query result: %s\n", cass_error_desc(rc));
		cass_future_free(query_future);
		return friend_list; // Return empty list on error
	}

	// Iterate through the rows in the result
	CassIterator *rows = cass_iterator_from_result(result);
	while (cass_iterator_next(rows))
	{
		const CassRow *row = cass_iterator_get_row(rows);

		// Retrieve the "friend" column value
		const char *friend_name;
		size_t friend_name_length;
		cass_value_get_string(cass_row_get_column_by_name(row, "user2"), &friend_name, &friend_name_length);

		// Add the friend to the list
		friend_list.emplace_back(friend_name, friend_name_length);
		cout << "Found friend for username " << username << ":" << friend_name << endl;
	}

	// Free resources
	cass_iterator_free(rows);
	cass_result_free(result);
	cass_future_free(query_future);

	return friend_list;
}

int Cassandra::get_user_from_db(const string &username, const string &password)
{
	// Prepare the query
	const char *query = "SELECT * FROM users WHERE username = ?;";
	CassStatement *statement = cass_statement_new(query, 1);

	// Bind the username to the query
	cass_statement_bind_string(statement, 0, username.c_str());

	// Execute the query
	CassFuture *query_future = cass_session_execute(session, statement);
	cass_statement_free(statement);

	// Handle the query result
	const CassResult *result = cass_future_get_result(query_future);
	if (result == NULL)
	{
		CassError rc = cass_future_error_code(query_future);
		printf("Query result: %s\n", cass_error_desc(rc));
		cass_future_free(query_future);
		return -1;
	}

	// Iterate through the rows in the result
	CassIterator *rows = cass_iterator_from_result(result);
	while (cass_iterator_next(rows))
	{
		const CassRow *row = cass_iterator_get_row(rows);

		// Extract password
		const char *queried_password;
		size_t password_length;
		cass_value_get_string(cass_row_get_column_by_name(row, "password"), &queried_password, &password_length);
		if (queried_password == password)
		{
			cout << "LOGIN: found user" << endl;
			cass_iterator_free(rows);
			cass_result_free(result);
			cass_future_free(query_future);

			return 1;
		}
	}

	// Free resources
	cass_iterator_free(rows);
	cass_result_free(result);
	cass_future_free(query_future);

	return -1;
}

string Cassandra::get_chat_history_from_db(string username, string _friend, int64_t paging_timestamp)
{
	ostringstream chat_history;
	cout << "Getting chat history from Cassandra for: " << username << " friend: " << _friend << " timestamp: " << paging_timestamp << endl;

	// Prepare the query
	const char *query = "SELECT user1, user2, sender, message, timestamp FROM conversation "
						"WHERE user1 = ? AND user2 = ? AND timestamp < ? "
						"ORDER BY timestamp DESC "
						"LIMIT 50;";

	vector<tuple<string, string, string, int64_t>> messages; // To store messages temporarily

	// Helper lambda to execute a query and collect results
	auto execute_query = [&](string user1, string user2)
	{
		CassStatement *statement = cass_statement_new(query, 3);
		cass_statement_bind_string(statement, 0, user1.c_str());
		cass_statement_bind_string(statement, 1, user2.c_str());
		cass_statement_bind_int64(statement, 2, paging_timestamp);

		CassFuture *query_future = cass_session_execute(session, statement);
		cass_statement_free(statement);

		const CassResult *result = cass_future_get_result(query_future);
		if (result == NULL)
		{
			CassError rc = cass_future_error_code(query_future);
			printf("Query result: %s\n", cass_error_desc(rc));
			cass_future_free(query_future);
			return;
		}

		CassIterator *rows = cass_iterator_from_result(result);
		while (cass_iterator_next(rows))
		{
			const CassRow *row = cass_iterator_get_row(rows);

			// Extract sender
			const char *sender_raw;
			size_t sender_raw_length;
			cass_value_get_string(cass_row_get_column_by_name(row, "sender"), &sender_raw, &sender_raw_length);

			// Extract message
			const char *message;
			size_t message_length;
			cass_value_get_string(cass_row_get_column_by_name(row, "message"), &message, &message_length);

			// Extract timestamp
			cass_int64_t timestamp;
			cass_value_get_int64(cass_row_get_column_by_name(row, "timestamp"), &timestamp);

			// Add to messages vector
			messages.emplace_back(string(sender_raw, sender_raw_length), string(message, message_length), user2, timestamp);
		}

		cass_iterator_free(rows);
		cass_result_free(result);
		cass_future_free(query_future);
	};

	// Execute the two queries
	execute_query(username, _friend);
	execute_query(_friend, username);

	// Sort messages by timestamp in ascending order
	sort(messages.begin(), messages.end(), [](const auto &a, const auto &b)
		 {
			 return get<3>(a) < get<3>(b); // Compare timestamps
		 });

	// Build the chat history string
	for (const auto &msg : messages)
	{
		chat_history << get<0>(msg) << " "	 // Sender
					 << get<2>(msg) << " "	 // Recipient
					 << get<1>(msg) << "\n"; // Message
	}

	// Append the last message timestamp if available
	if (!messages.empty())
	{
		chat_history << get<3>(messages.back());
	}

	cout << "Returning chat history: " << chat_history.str() << endl;
	return chat_history.str();
}

// string Cassandra::get_chat_history_from_db(string username, string _friend, int64_t paging_timestamp)
// {
// 	ostringstream chat_history;
// 	cout << "Getting chat history from Cassandra for: " << username << " friend: " << _friend << " timestamp: " << paging_timestamp << endl;

// 	// Prepare the query with a timestamp filter for paging
// 	const char *query = "SELECT user1, user2, sender, message, timestamp FROM conversation "
// 						"WHERE user1 = ? AND user2 = ? AND timestamp < ? "
// 						"ORDER BY timestamp DESC "
// 						"LIMIT 50;";
// 	CassStatement *statement = cass_statement_new(query, 3);

// 	// Bind the parameters
// 	cass_statement_bind_string(statement, 0, username.c_str());
// 	cass_statement_bind_string(statement, 1, _friend.c_str());
// 	cass_statement_bind_int64(statement, 2, paging_timestamp);

// 	// Execute the query
// 	CassFuture *query_future = cass_session_execute(session, statement);
// 	cass_statement_free(statement);

// 	// Handle the query result
// 	const CassResult *result = cass_future_get_result(query_future);
// 	if (result == NULL)
// 	{
// 		CassError rc = cass_future_error_code(query_future);
// 		printf("Query result: %s\n", cass_error_desc(rc));
// 		cass_future_free(query_future);
// 		return "";
// 	}

// 	// Iterate through the rows in the result
// 	CassIterator *rows = cass_iterator_from_result(result);
// 	cass_int64_t returned_timestamp = -1;
// 	while (cass_iterator_next(rows))
// 	{
// 		const CassRow *row = cass_iterator_get_row(rows);

// 		// Extract sender
// 		const char *sender_raw;
// 		size_t sender_raw_length;
// 		cass_value_get_string(cass_row_get_column_by_name(row, "sender"), &sender_raw, &sender_raw_length);

// 		// Extract message
// 		const char *message;
// 		size_t message_length;
// 		cass_value_get_string(cass_row_get_column_by_name(row, "message"), &message, &message_length);

// 		// Extract timestamp
// 		cass_value_get_int64(cass_row_get_column_by_name(row, "timestamp"), &returned_timestamp);

// 		// Append to chat history
// 		// SENDER RECIPIENT MESSAGE TIMESTAMP
// 		string sender = string(sender_raw, sender_raw_length);
// 		string recipient;
// 		if (sender == username)
// 		{
// 			recipient = _friend;
// 		}
// 		else
// 		{
// 			recipient = username;
// 		}

// 		// first include the last chat message timestamp
// 		// SENDER RECIPIENT MESSAGE\nSENDER RECIPIENT MESSAGE\nTIMESTAMP
// 		chat_history << sender << " "
// 					 << recipient << " " << string(message, message_length) << "\n";
// 	}

// 	if (returned_timestamp != -1)
// 	{
// 		chat_history << returned_timestamp;
// 	}

// 	// Free resources
// 	cass_iterator_free(rows);
// 	cass_result_free(result);
// 	cass_future_free(query_future);

// 	cout << "Returning chat history: " << chat_history.str() << endl;
// 	return chat_history.str();
// }
