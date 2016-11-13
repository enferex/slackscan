CC = gcc
CFLAGS = -g3 -O0 -lext2fs -lcom_err -Wunused
APP = slackscan

$(APP): main.c
	$(CC) -o $@ $^ $(CFLAGS)

debug: $(APP)
	sudo gdb --args ./$(APP) -d /dev/sda1

test: $(APP)
	sudo ./$(APP) -d /dev/sda1
	sudo ./$(APP) -f ./$(APP)

clean:
	$(RM) $(APP)
