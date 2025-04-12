ALL: producer consumer

CFLAGS=-Wall $(shell pkg-config --cflags glib-2.0 rdkafka)
LDLIBS=$(shell pkg-config --libs glib-2.0 rdkafka)

producer: producer.o
	g++ $(CFLAGS) -o producer producer.o $(LDLIBS)

consumer: consumer.o
	g++ $(CFLAGS) -o consumer consumer.o $(LDLIBS)

producer.o: producer.cc
	g++ $(CFLAGS) -c producer.cc

consumer.o: consumer.cc
	g++ $(CFLAGS) -c consumer.cc