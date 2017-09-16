NAME = expression-pedal
BUILD_DIR = ./build
LIBS = `pkg-config --cflags --libs jack`

ifeq ($(DEBUG),Y)
	C_FLAGS = -g -Og -DDEBUG
else
	C_FLAGS = -g -O2
endif

all: clean $(NAME)

$(NAME):
	mkdir -p $(BUILD_DIR)
	gcc -std=c11 src/main.c -Wno-unused-parameter $(LIBS) \
		-o $(BUILD_DIR)/$(NAME) $(C_FLAGS)

clean:
	rm -rf "$(BUILD_DIR)/$(NAME)"
