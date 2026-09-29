# Program for compiling C programs
CC		= c++
# Extra flags to give to the C preprocessor and programs that use it
CPPFLAGS= -I include
# Extra flags to give to the processor C compiler
CFLAGS	= -Wall -Werror -Wextra -std=c++98 -g
# Optimization flag to give to the processor C compiler
COPTFLAGS	= -O3

#
SRC		= src/main.cpp \
			src/config/ConfigParser.cpp \
			src/config/Location.cpp \
			src/server/Server.cpp \
			src/http/Request.cpp \
			src/http/Response.cpp
OBJ		= $(SRC:.cpp=.o)
NAME	= webserv
#
.PHONY: all clean fclean re

%.o: %.cpp
	$(CC) -c $(CFLAGS) $(COPTFLAGS) $(CPPFLAGS) $< -o $@

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) $(COPTFLAGS) -o $@ $(OBJ) $(LDLIBS)

clean:
	rm -f $(OBJ)

fclean: clean
	rm -f $(NAME)

re: fclean all
