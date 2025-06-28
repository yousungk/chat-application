TARGETS = backend_server frontend_server frontend_server_real

CFLAGS = -Wall -std=c++20 -pthread $(shell pkg-config --cflags rdkafka glib-2.0) -Ibackend -I/opt/homebrew/include
LDLIBS = $(shell pkg-config --libs rdkafka glib-2.0) -L/opt/homebrew/lib -lcassandra

all: $(TARGETS)

backend_server: backend/backend_server.cc
	g++ $(CFLAGS) backend/*.cc -o backend_server $(LDLIBS)

frontend_server: frontend/frontend_server.cc
	g++ -Wall -std=c++20 -pthread frontend/frontend_server.cc -o frontend_server

frontend_server_real: frontend/frontend_server_real.cc
	g++ -Wall -std=c++20 -pthread -Ibackend -I/opt/homebrew/include -I/opt/homebrew/Cellar/boost/1.88.0/include frontend/frontend_server_real.cc frontend/tcp_pool.cc backend/cassandra_client.cc -o frontend_server_real -L/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_system -lboost_thread -lboost_json -L/opt/homebrew/lib -lcassandra

clean:
	rm -fv $(TARGETS) *~ ; rm -fv *.o