all: server client

server:tcp_svr.cc
	g++ -o $@ $^ -std=c++17 -pthread
client:tcp_cli.cc	
	g++ -o $@ $^ -std=c++17 -pthread

.PHONY:clean
clean:
	rm -f server client