# Fairly generic cross-compilation makefile for simple programs
CC=$(CROSSTOOL)/$(ARM)/bin/gcc
NAME=rico

all: $(NAME)
	$(CROSSTOOL)/$(ARM)/bin/strip $(NAME)
	mv $(NAME) $(NAME).new

$(NAME): $(NAME).c
