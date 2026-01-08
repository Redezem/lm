lm:
	c++ -std=c++20 -O2 -Wall -Wextra -I/usr/local/include -L/usr/local/lib main.cpp -lcurl -lncurses -o lm

test:
	c++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -I/usr/local/include -L/usr/local/lib tests/test_main.cpp -lcurl -lncurses -o lm_test
	./lm_test

clean:
	rm lm lm_test
