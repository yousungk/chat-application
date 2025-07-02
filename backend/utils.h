#ifndef UTILS_H
#define UTILS_H

#include <string.h>
#include <vector>

using namespace std;

int get_partition(string key);
vector<string> split(string message, char delimiter);
string get_address(struct sockaddr_in &addr);
void do_write(int fd, const char *buf, int len);
string do_read(int fd, string delim);
void print_debug(bool debug_print, const char *message, ...);

#endif