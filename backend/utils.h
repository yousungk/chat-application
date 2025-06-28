#ifndef UTILS_H
#define UTILS_H

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

using namespace std;

int get_partition(string key);
vector<string> split(string message, char delimiter);
std::string get_address(struct sockaddr_in &addr);
void do_write(int fd, const char *buf, int len);
std::string do_read(int fd, std::string delim);
void print_debug(bool debug_print, const char *message, ...);

#endif