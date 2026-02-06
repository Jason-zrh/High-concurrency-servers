all:server client
server:server.cc
	g++ -o $@ $^ -std=c++20
client:client.cc	
	g++ -o $@ $^ -std=c++20

.PHONY:clean
clean:
	rm -f server client