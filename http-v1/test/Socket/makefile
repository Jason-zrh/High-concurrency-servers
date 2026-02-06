all: server client

server:tcp_svr.cc
	g++ -o $@ $^ -std=c++11

client:tcp_cli.cc	
	g++ -o $@ $^ -std=c++11

.PHONY:clean	
clean:
	rm -f server client