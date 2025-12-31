lm:
	c++ -std=c++20 -O2 -Wall -Wextra -I/usr/local/include -L/usr/local/lib main.cpp -lcurl -o lm

clean:
	rm lm
