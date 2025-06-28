#ifndef CASSANDRA_H
#define CASSANDRA_H

#include <cassandra.h>
#include <iostream>
#include <stdio.h>

using namespace std;

class Cassandra
{
private:
	CassFuture *connect_future;
	CassCluster *cluster;
	CassSession *session;

public:
	Cassandra(string address);
	~Cassandra();
	int save_message_to_db(const string &user1, const string &user2, const string &sender, const string &message);
	int save_user_to_db(const string &username, const string &password);
	int save_friend_to_db(const string &username, const string &_friend);
	vector<string> get_friend_list_from_db(const string &username);
	int get_user_from_db(const string &username, const string &password);
	string get_chat_history_from_db(string username, string _friend, int64_t paging_timestamp);
};

#endif